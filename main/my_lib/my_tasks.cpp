#include "my_tasks.h"
#include "glb_params.h"
#include "help_functions.h"
#include "auto_eq_help.h"
#include "config_functions.h"
#include "my_usb_device.h"

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
    configure_spi();
    if (spi_hdl == NULL) {
        ESP_LOGE(SAMPLING_TAG, "SPI handle not available, aborting sample task");
        sample_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    spi_transaction_t spi_t {
        .flags      = SPI_TRANS_USE_RXDATA,
        .length     = TRANSACTION_LENGTH,
        .rxlength   = TRANSACTION_LENGTH,
    };

    uint16_t *samples = (uint16_t *)heap_caps_malloc(N_SAMPLES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (samples == NULL) {
        ESP_LOGE(SAMPLING_TAG, "Failed to allocate sample buffer");
        sample_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    xEventGroupSync(
        sync_tasks,
        TASK_A_READY_BIT,
        ALL_TASKS_READY,
        portMAX_DELAY
    );

    uint32_t t_start = esp_log_timestamp();
    for (int i = 0; i < N_SAMPLES; i++) {
        spi_device_polling_transmit(spi_hdl, &spi_t);
        uint32_t raw_res = ((uint32_t)spi_t.rx_data[0] << 16) |
                           ((uint32_t)spi_t.rx_data[1] <<  8) |
                            (uint32_t)spi_t.rx_data[2];
        samples[i] = (raw_res >> 2) & 0xFFFF;
    }
    uint32_t t_end = esp_log_timestamp();
    (void)t_start;
    (void)t_end;

    float actual_fs = (float)N_SAMPLES / ((t_end - t_start) / 1000.0f);
    (void)actual_fs;

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

void vPlay_WAV_task(void* args)
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
    bool using_fallback_mono = false;

    wav_hdl = wave_reader_open(primary_wav);
    if (wav_hdl == NULL) {
        ESP_LOGW(WAV_TAG, "Primary WAV missing, falling back to %s", fallback_wav);
        wav_hdl = wave_reader_open(fallback_wav);
        using_fallback_mono = (wav_hdl != NULL);
    }

    if (wav_hdl == NULL) {
        ESP_LOGE(WAV_TAG, "Unable to open read!");
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    if (wave_read_header(wav_hdl, &wav_head) != 0) {
        ESP_LOGE(WAV_TAG, "Unable to read WAV file header!");
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    const size_t mono_chunk_bytes = BUFFER_BYTES / 2;
    uint8_t *out_buff = (uint8_t *)calloc(1, BUFFER_BYTES);
    uint8_t *mono_buff = using_fallback_mono ? (uint8_t *)calloc(1, mono_chunk_bytes) : NULL;

    if (out_buff == NULL || (using_fallback_mono && mono_buff == NULL)) {
        ESP_LOGE(WAV_TAG, "Failed to allocate WAV playback buffers");
        if (mono_buff != NULL) {
            free(mono_buff);
        }
        if (out_buff != NULL) {
            free(out_buff);
        }
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        wav_task_running = false;
        finish_play_test_if_done();
        vTaskDelete(NULL);
        return;
    }

    size_t wrote = 0;
    size_t pos = 0;

    ESP_LOGI(WAV_TAG, "Starting WAV playback%s",
             using_fallback_mono ? " with mono-to-stereo expansion" : "");

    xEventGroupSync(
        sync_tasks,
        TASK_B_READY_BIT,
        ALL_TASKS_READY,
        portMAX_DELAY
    );

    esp_err_t enable_tx_err = i2s_channel_enable(mcu_tx);
    if (enable_tx_err != ESP_OK) {
        ESP_LOGE(WAV_TAG, "Failed to enable I2S TX channel: %s", esp_err_to_name(enable_tx_err));
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        free(out_buff);
        if (mono_buff != NULL) {
            free(mono_buff);
        }
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
            wave_reader_close(wav_hdl);
            wav_hdl = NULL;
            free(out_buff);
            if (mono_buff != NULL) {
                free(mono_buff);
            }
            wav_task_running = false;
            finish_play_test_if_done();
            vTaskDelete(NULL);
            return;
        }
    }

    const uint32_t PLAY_DURATION_MS = 5000;
    uint32_t t_start = esp_log_timestamp();

    while ((esp_log_timestamp() - t_start) < PLAY_DURATION_MS)
    {
        size_t bytes_read = 0;
        size_t bytes_to_write = 0;

        if (using_fallback_mono) {
            bytes_read = wave_read_raw_data(wav_hdl, mono_buff, pos, mono_chunk_bytes);
            if (bytes_read == 0) {
                break;
            }
            pos += bytes_read;
            expand_mono16_to_stereo16(mono_buff, bytes_read, out_buff);
            bytes_to_write = bytes_read * 2;
        } else {
            bytes_read = wave_read_raw_data(wav_hdl, out_buff, pos, BUFFER_BYTES);
            if (bytes_read == 0) {
                break;
            }
            pos += bytes_read;
            bytes_to_write = bytes_read;
        }

        uint8_t *p = out_buff;
        size_t bytes_remaining = bytes_to_write;

        while (bytes_remaining > 0) {
            wrote = 0;
            uint32_t elapsed = esp_log_timestamp() - t_start;
            uint32_t remaining = (elapsed < PLAY_DURATION_MS) ? (PLAY_DURATION_MS - elapsed) : 0;

            esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_remaining, &wrote, pdMS_TO_TICKS(remaining));
            if (r != ESP_OK) {
                ESP_LOGE(WAV_TAG, "i2s_channel_write failed: %s", esp_err_to_name(r));
                bytes_remaining = 0;
                break;
            }

            if (wrote == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            bytes_remaining -= wrote;
            p += wrote;
        }
    }

    if (mcu_rx != NULL) {
        i2s_channel_disable(mcu_rx);
    }
    i2s_channel_disable(mcu_tx);
    wave_reader_close(wav_hdl);
    wav_hdl = NULL;
    free(out_buff);
    if (mono_buff != NULL) {
        free(mono_buff);
    }

    ESP_LOGI(WAV_TAG, "WAV playback task finished");

    wav_task_running = false;
    finish_play_test_if_done();
    vTaskDelete(NULL);
}

void vUSB_playback_task(void *arg)
{
    usb_uac_device_init();
    configure_i2s_for_audio();
    i2s_channel_enable(mcu_tx);

    while (1) {
        size_t bytes_received = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
            audio_ringbuf, &bytes_received, portMAX_DELAY, 192);
        if (data) {
            size_t bytes_written = 0;
            i2s_channel_write(mcu_tx, data, bytes_received, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(audio_ringbuf, data);
        }
    }

    i2s_channel_disable(mcu_tx);
    vTaskDelete(NULL);
}

void vBT_playback_task(void *arg)
{
    bm83_tx_ind_init();
    while (1) {
        int level = gpio_get_level(MCU_WAKE);
        if (level == 0) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(BM83_TAG, "BM83 Transmitting!");
    configure_i2s_for_audio();

    i2s_channel_enable(mcu_tx);
    i2s_channel_enable(mcu_rx);

    uint8_t *bt_buff = (uint8_t *)calloc(1, BUFFER_BYTES);
    assert(bt_buff);

    size_t bytes_read;
    size_t wrote = 0;

    while (1)
    {
        i2s_channel_read(mcu_rx, bt_buff, BUFFER_BYTES, &bytes_read, portMAX_DELAY);

        size_t bytes_to_w = bytes_read;
        uint8_t *p = bt_buff;

        while (bytes_to_w > 0)
        {
            esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY);

            if (r != ESP_OK) {
                ESP_LOGE(I2S_TAG, "I2S threw ERROR: %d", r);
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
    i2s_channel_disable(mcu_tx);
    i2s_channel_disable(mcu_rx);
    vTaskDelete(NULL);
}
