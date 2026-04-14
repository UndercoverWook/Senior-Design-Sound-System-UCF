#include "my_tasks.h"
#include "glb_params.h"
#include "help_functions.h"
#include "auto_eq_help.h"
#include "config_functions.h"
#include "my_usb_device.h"

#include <algorithm>
#include <cstring>

static inline uint32_t read_cpu_cycle_count()
{
    uint32_t ccount;
    __asm__ __volatile__("rsr.ccount %0" : "=a"(ccount));
    return ccount;
}

static void finish_play_test_if_done()
{
    if (!sample_task_running && !wav_task_running) {
        play_test_running = false;

        if (sync_tasks != NULL) {
            vEventGroupDelete(sync_tasks);
            sync_tasks = NULL;
        }

        task1_hdl = NULL;
        task2_hdl = NULL;
    }
}

void vSample_task(void *args)
{
    if (uxTaskPriorityGet(NULL) > 1) {
        vTaskPrioritySet(NULL, uxTaskPriorityGet(NULL) - 1);
    }

    configure_spi();
    if (spi_hdl == NULL) {
        ESP_LOGE(SAMPLING_TAG, "SPI handle not available, aborting sample task");
        sample_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    spi_transaction_t spi_t{
        .flags = SPI_TRANS_USE_RXDATA,
        .length = TRANSACTION_LENGTH,
        .rxlength = TRANSACTION_LENGTH,
    };

    uint16_t *samples = (uint16_t *)heap_caps_malloc(N_SAMPLES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (samples == NULL) {
        ESP_LOGE(SAMPLING_TAG, "Failed to allocate sample buffer");
        sample_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    xEventGroupSync(sync_tasks, TASK_A_READY_BIT, ALL_TASKS_READY, portMAX_DELAY);

    const uint64_t cpu_ticks_per_us = (uint64_t)esp_rom_get_cpu_ticks_per_us();
    const uint32_t cycles_per_sample =
        (uint32_t)((cpu_ticks_per_us * 1000000ULL + (SAMPLE_RATE / 2)) / SAMPLE_RATE);
    uint32_t next_cycle = read_cpu_cycle_count();

    uint32_t t_start = esp_log_timestamp();
    for (int i = 0; i < N_SAMPLES; i++) {
        while ((int32_t)(read_cpu_cycle_count() - next_cycle) < 0) {
            ;
        }
        next_cycle += cycles_per_sample;

        esp_err_t err = spi_device_polling_transmit(spi_hdl, &spi_t);
        if (err != ESP_OK) {
            ESP_LOGE(SAMPLING_TAG, "SPI sample transfer failed at index %d: %s", i, esp_err_to_name(err));
            break;
        }

        uint32_t raw_res = ((uint32_t)spi_t.rx_data[0] << 16) |
                           ((uint32_t)spi_t.rx_data[1] << 8) |
                           (uint32_t)spi_t.rx_data[2];
        samples[i] = (raw_res >> 2) & 0xFFFF;
    }
    uint32_t t_end = esp_log_timestamp();

    float actual_fs = (float)N_SAMPLES / std::max(0.001f, ((t_end - t_start) / 1000.0f));
    ESP_LOGI(SAMPLING_TAG, "Timed sampling finished, measured Fs = %.2f Hz", actual_fs);

    free(samples);
    sample_task_running = false;
    finish_play_test_if_done();
    vTaskDelete(NULL);
}

static void expand_mono16_to_stereo16(const uint8_t *mono_in,
                                      size_t mono_bytes,
                                      uint8_t *stereo_out)
{
    const int16_t *src = (const int16_t *)mono_in;
    int16_t *dst = (int16_t *)stereo_out;
    const size_t mono_samples = mono_bytes / sizeof(int16_t);

    for (size_t i = 0; i < mono_samples; ++i) {
        const int16_t s = src[i];
        dst[2 * i] = s;
        dst[2 * i + 1] = s;
    }
}

void vPlay_WAV_task(void *args)
{
    configure_i2s_for_wav();
    if (mcu_tx == NULL) {
        ESP_LOGE(WAV_TAG, "I2S TX handle not available, aborting WAV task");
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    wave_header_t wav_head;
    const char *primary_wav = "/storage/44k_full_sweep.wav";
    const char *fallback_wav = "/storage/1khz_sine_44_1.wav";

    wav_hdl = wave_reader_open(primary_wav);
    if (wav_hdl == NULL) {
        ESP_LOGW(WAV_TAG, "Primary WAV missing, falling back to %s", fallback_wav);
        wav_hdl = wave_reader_open(fallback_wav);
    }

    if (wav_hdl == NULL) {
        ESP_LOGE(WAV_TAG, "Unable to open WAV file");
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    if (wave_read_header(wav_hdl, &wav_head) != 0) {
        ESP_LOGE(WAV_TAG, "Unable to read WAV file header");
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    if (wav_head.bytes_per_sample != 2) {
        ESP_LOGE(WAV_TAG, "Only 16-bit PCM WAV is supported");
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    if (wav_head.n_channels != 1 && wav_head.n_channels != 2) {
        ESP_LOGE(WAV_TAG, "Unsupported channel count: %u", wav_head.n_channels);
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    const bool input_is_mono = (wav_head.n_channels == 1);
    const size_t in_frame_bytes =
        (size_t)wav_head.bytes_per_sample * (size_t)wav_head.n_channels;

    const size_t max_frames_to_play =
        std::min<size_t>((size_t)wav_head.samples_per_channel,
                         (size_t)wav_head.sample_rate * TEST_DURATION);

    const size_t max_input_bytes = max_frames_to_play * in_frame_bytes;

    uint8_t *preloaded_pcm = (uint8_t *)heap_caps_malloc(max_input_bytes, MALLOC_CAP_SPIRAM);
    if (preloaded_pcm == NULL) {
        preloaded_pcm = (uint8_t *)calloc(1, max_input_bytes);
    }
    if (preloaded_pcm == NULL) {
        ESP_LOGE(WAV_TAG, "Failed to allocate preload buffer");
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    size_t preload_pos = 0;
    const size_t preload_chunk = input_is_mono ? (BUFFER_BYTES / 2) : BUFFER_BYTES;
    while (preload_pos < max_input_bytes) {
        const size_t request = std::min(preload_chunk, max_input_bytes - preload_pos);
        size_t bytes_read = wave_read_raw_data(wav_hdl, preloaded_pcm + preload_pos, preload_pos, request);
        if (bytes_read == 0) {
            break;
        }
        preload_pos += bytes_read;
        if (bytes_read < request) {
            break;
        }
    }

    wave_reader_close(wav_hdl);
    wav_hdl = NULL;

    if (preload_pos == 0) {
        ESP_LOGE(WAV_TAG, "No PCM payload was loaded from WAV file");
        free(preloaded_pcm);
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    preload_pos -= (preload_pos % in_frame_bytes);
    const size_t frames_loaded = preload_pos / in_frame_bytes;

    static uint8_t mono_expand_buffer[BUFFER_BYTES];
    uint8_t *out_buff = input_is_mono ? mono_expand_buffer : NULL;

    ESP_LOGI(WAV_TAG,
             "Starting WAV playback%s from preloaded buffer (%u frames, %u Hz, %u channels)",
             input_is_mono ? " with mono-to-stereo expansion" : "",
             (unsigned)frames_loaded,
             (unsigned)wav_head.sample_rate,
             (unsigned)wav_head.n_channels);

    xEventGroupSync(sync_tasks, TASK_B_READY_BIT, ALL_TASKS_READY, portMAX_DELAY);

    esp_err_t enable_tx_err = i2s_channel_enable(mcu_tx);
    if (enable_tx_err != ESP_OK) {
        ESP_LOGE(WAV_TAG, "Failed to enable I2S TX channel: %s", esp_err_to_name(enable_tx_err));
        free(preloaded_pcm);
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    if (mcu_rx != NULL) {
        esp_err_t enable_rx_err = i2s_channel_enable(mcu_rx);
        if (enable_rx_err != ESP_OK) {
            ESP_LOGE(WAV_TAG, "Failed to enable I2S RX channel: %s", esp_err_to_name(enable_rx_err));
            i2s_channel_disable(mcu_tx);
            free(preloaded_pcm);
            wav_task_running = false;
            finish_play_test_if_done();
            vTaskDelete(NULL);
            return;
        }
    }

    size_t frame_pos = 0;
    while (frame_pos < frames_loaded) {
        const uint8_t *write_ptr = NULL;
        size_t bytes_to_write = 0;

        if (input_is_mono) {
            const size_t frames_remaining = frames_loaded - frame_pos;
            const size_t frames_take = std::min((size_t)(BUFFER_BYTES / FRAME_SIZE_BYTES), frames_remaining);
            const size_t mono_take_bytes = frames_take * sizeof(int16_t);

            expand_mono16_to_stereo16(
                preloaded_pcm + (frame_pos * sizeof(int16_t)),
                mono_take_bytes,
                out_buff);

            write_ptr = out_buff;
            bytes_to_write = frames_take * FRAME_SIZE_BYTES;
            frame_pos += frames_take;
        } else {
            const size_t frames_remaining = frames_loaded - frame_pos;
            const size_t frames_take = std::min((size_t)(BUFFER_BYTES / FRAME_SIZE_BYTES), frames_remaining);

            write_ptr = preloaded_pcm + (frame_pos * FRAME_SIZE_BYTES);
            bytes_to_write = frames_take * FRAME_SIZE_BYTES;
            frame_pos += frames_take;
        }

        while (bytes_to_write > 0) {
            size_t wrote = 0;
            esp_err_t r = i2s_channel_write(mcu_tx, write_ptr, bytes_to_write, &wrote, portMAX_DELAY);
            if (r != ESP_OK) {
                ESP_LOGE(WAV_TAG, "i2s_channel_write failed: %s", esp_err_to_name(r));
                bytes_to_write = 0;
                break;
            }

            if (wrote == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            bytes_to_write -= wrote;
            write_ptr += wrote;
        }
    }

    if (mcu_rx != NULL) {
        i2s_channel_disable(mcu_rx);
    }
    i2s_channel_disable(mcu_tx);

    free(preloaded_pcm);

    ESP_LOGI(WAV_TAG, "WAV playback task finished");

    wav_task_running = false;
    finish_play_test_if_done();
    vTaskDelete(NULL);
}

void vUSB_playback_task(void *arg)
{
    (void)arg;

    usb_uac_device_init();
    configure_i2s_for_audio();
    if (audio_tx == NULL) {
        ESP_LOGE(I2S_TAG, "USB/BM83 audio TX handle is NULL");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(i2s_channel_enable(audio_tx));

    while (1) {
        size_t bytes_received = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(audio_ringbuf, &bytes_received, portMAX_DELAY, 192);
        if (data) {
            size_t bytes_written = 0;
            i2s_channel_write(audio_tx, data, bytes_received, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(audio_ringbuf, data);
        }
    }

    i2s_channel_disable(audio_tx);
    if (audio_rx != NULL) {
        i2s_channel_disable(audio_rx);
    }
    vTaskDelete(NULL);
}

void vBT_playback_task(void *arg)
{
    (void)arg;

    bm83_tx_ind_init();
    ESP_LOGI(BM83_TAG, "BM83 playback task started");
    ESP_LOGI(BM83_TAG, "Configuring BM83 audio bridge immediately...");

    configure_i2s_for_audio();
    if (audio_tx == NULL || audio_rx == NULL) {
        ESP_LOGE(I2S_TAG, "BM83 audio handles not available after configure_i2s_for_audio()");
        vTaskDelete(NULL);
        return;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2S_RX_LINE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    bool bridge_enabled = false;
    bool seen_nonzero_audio = false;
    int consecutive_zero_buffers = 0;

    uint8_t *bt_buff = (uint8_t *)calloc(1, BUFFER_BYTES);
    if (bt_buff == NULL) {
        ESP_LOGE(BM83_TAG, "Failed to allocate BM83 audio buffer");
        vTaskDelete(NULL);
        return;
    }
        /* teammate path */
    esp_rom_gpio_connect_out_signal(I2S_RX_LINE, 0x100, false, false);
    esp_rom_gpio_connect_in_signal(I2S_RX_LINE, 25, false);
    gpio_set_drive_capability(I2S_BIT_CLK, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(I2S_LRCLK_PIN, GPIO_DRIVE_CAP_0);
    while (1)
    {
        /*
         * WAV playback owns the DAC path while the play test runs.
         * Pause the BM83 bridge, then resume it automatically afterward.
         */
        if (play_test_running) {
            if (bridge_enabled) {
                i2s_channel_disable(audio_tx);
                i2s_channel_disable(audio_rx);
                bridge_enabled = false;
                ESP_LOGI(BM83_TAG, "BM83 bridge paused while Play Test Tone is active");
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!bridge_enabled) {
            ESP_ERROR_CHECK(i2s_channel_enable(audio_tx));
            ESP_ERROR_CHECK(i2s_channel_enable(audio_rx));
            bridge_enabled = true;
            ESP_LOGI(BM83_TAG, "BM83 I2S bridge enabled");
        }

        size_t bytes_read = 0;
        esp_err_t rr = i2s_channel_read(audio_rx, bt_buff, BUFFER_BYTES, &bytes_read, pdMS_TO_TICKS(250));
        if (rr == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (rr != ESP_OK) {
            ESP_LOGE(I2S_TAG, "BM83 i2s_channel_read failed: %s", esp_err_to_name(rr));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (bytes_read == 0) {
            continue;
        }

        bool any_nonzero = false;
        for (size_t i = 0; i < bytes_read; ++i) {
            if (bt_buff[i] != 0) {
                any_nonzero = true;
                break;
            }
        }

        if (!any_nonzero) {
            consecutive_zero_buffers++;
            if ((consecutive_zero_buffers % 25) == 0) {
                ESP_LOGW(BM83_TAG, "BM83 received %d consecutive all-zero buffers", consecutive_zero_buffers);
            }
            continue;
        }

        consecutive_zero_buffers = 0;

        if (!seen_nonzero_audio) {
            ESP_LOGI(BM83_TAG, "BM83 non-zero audio stream detected (%u bytes)", (unsigned)bytes_read);
            seen_nonzero_audio = true;
        }

        size_t bytes_to_w = bytes_read;
        uint8_t *p = bt_buff;

        while (bytes_to_w > 0)
        {
            size_t wrote = 0;
            esp_err_t r = i2s_channel_write(audio_tx, p, bytes_to_w, &wrote, portMAX_DELAY);

            if (r != ESP_OK) {
                ESP_LOGE(I2S_TAG, "BM83 i2s_channel_write failed: %s", esp_err_to_name(r));
                break;
            }

            if (wrote == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            bytes_to_w -= wrote;
            p += wrote;
        }
    }

    free(bt_buff);
    if (bridge_enabled) {
        i2s_channel_disable(audio_tx);
        i2s_channel_disable(audio_rx);
    }
    vTaskDelete(NULL);
}
