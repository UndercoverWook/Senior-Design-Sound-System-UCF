#include "my_tasks.h"
#include "glb_params.h"
#include "help_functions.h"
#include "auto_eq_help.h"
#include "config_functions.h"
#include "my_usb_device.h"
#include "ble_control.h"
#include <math.h>
#include <string.h>

static bool is_task_suspendable(TaskHandle_t task)
{
    if (task == NULL) {
        return false;
    }
    eTaskState state = eTaskGetState(task);
    return state != eDeleted && state != eInvalid;
}

static bool load_and_expand_wav_to_buffer(
    wave_reader_handle_t wav,
    uint32_t wav_channels,
    bool expand_mono_to_stereo,
    size_t target_input_bytes,
    uint8_t *stereo_out,
    size_t stereo_out_capacity,
    size_t *actual_input_bytes,
    size_t *actual_output_bytes)
{
    if (!wav || !stereo_out || !actual_input_bytes || !actual_output_bytes) {
        return false;
    }

    uint8_t *read_buf = (uint8_t *)heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (read_buf == NULL) {
        return false;
    }

    size_t pos = 0;
    size_t total_in = 0;
    size_t total_out = 0;
    const size_t max_input_chunk = expand_mono_to_stereo ? (BUFFER_BYTES / 2U) : BUFFER_BYTES;

    while (total_in < target_input_bytes && total_out < stereo_out_capacity) {
        size_t input_left = target_input_bytes - total_in;
        size_t read_size = (input_left < max_input_chunk) ? input_left : max_input_chunk;
        size_t bytes_read = wave_read_raw_data(wav, read_buf, pos, read_size);
        if (bytes_read == 0) {
            break;
        }

        pos += bytes_read;
        total_in += bytes_read;

        if (expand_mono_to_stereo) {
            size_t sample_count = bytes_read / sizeof(int16_t);
            if ((total_out + (sample_count * FRAME_SIZE_BYTES)) > stereo_out_capacity) {
                free(read_buf);
                return false;
            }

            const int16_t *src = (const int16_t *)read_buf;
            int16_t *dst = (int16_t *)(stereo_out + total_out);
            for (size_t i = 0; i < sample_count; ++i) {
                dst[(i * 2U) + 0U] = src[i];
                dst[(i * 2U) + 1U] = src[i];
            }
            total_out += sample_count * FRAME_SIZE_BYTES;
        } else {
            if ((total_out + bytes_read) > stereo_out_capacity) {
                free(read_buf);
                return false;
            }
            memcpy(stereo_out + total_out, read_buf, bytes_read);
            total_out += bytes_read;
        }
    }

    free(read_buf);
    *actual_input_bytes = total_in;
    *actual_output_bytes = total_out;
    return true;
}

static void finish_calibration_run(bool success)
{
    EventGroupHandle_t eg = sync_tasks;
    sync_tasks = NULL;

    calibration_in_progress = false;
    if (success) {
        ble_send_app_message("CAL_DONE");
    } else {
        ble_send_app_message("CAL_FAILED");
    }

    if (eg != NULL) {
        vEventGroupDelete(eg);
    }

    if (bt_task && eTaskGetState(bt_task) == eSuspended) {
        vTaskResume(bt_task);
    }
}

static void reset_filter_states(void)
{
    memset(eq_w, 0, sizeof(eq_w));
    memset(sub_lpf_w, 0, sizeof(sub_lpf_w));
    memset(mid_hpf_w, 0, sizeof(mid_hpf_w));
}

static void refresh_filter_coeffs_if_needed(bool force_reset_states)
{
    static bool coeffs_initialized = false;
    static float last_gains[EQ_BANDS] = {0};

    bool gains_changed = force_reset_states || !coeffs_initialized;
    if (!gains_changed) {
        for (int i = 0; i < EQ_BANDS; i++) {
            if (fabsf(app_sliders[i] - last_gains[i]) > 0.01f) {
                gains_changed = true;
                break;
            }
        }
    }
    if (!gains_changed) return;

    const float crossover_f = 250.0f;
    dsps_biquad_gen_lpf_f32(lpf_coeffs, crossover_f / SAMPLE_RATE, 0.707f);
    dsps_biquad_gen_hpf_f32(hpf_coeffs, crossover_f / SAMPLE_RATE, 0.707f);
    for (int i = 0; i < EQ_BANDS; i++) {
        my_dsps_biquad_gen_peakingEQ_f32(eq_coeffs[i], eq_freqs[i] / SAMPLE_RATE, app_sliders[i], 1.0f);
        last_gains[i] = app_sliders[i];
    }
    coeffs_initialized = true;
    reset_filter_states();
}

static inline int16_t clamp_to_i16(float x)
{
    if (x > 32767.0f) return 32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)x;
}

static void process_stereo_pcm_inplace(int16_t *pcm, size_t frame_count)
{
    if (pcm == NULL || frame_count == 0 || !activate_eq) return;
    for (size_t i = 0; i < frame_count; i++) {
        float left_in = (float)pcm[i * 2 + 0] / 32768.0f;
        float right_in = (float)pcm[i * 2 + 1] / 32768.0f;
        float mono_sample = (left_in + right_in) * 0.35f;
        float eq_sample = mono_sample;
        for (int b = 0; b < EQ_BANDS; b++) {
            float out = 0.0f;
            dsps_biquad_f32_aes3(&eq_sample, &out, 1, eq_coeffs[b], eq_w[b]);
            eq_sample = out;
        }
        float sub_sample = 0.0f;
        float mid_sample = 0.0f;
        dsps_biquad_f32_aes3(&eq_sample, &sub_sample, 1, lpf_coeffs, sub_lpf_w);
        dsps_biquad_f32_aes3(&eq_sample, &mid_sample, 1, hpf_coeffs, mid_hpf_w);
        pcm[i * 2 + 0] = clamp_to_i16(mid_sample * 32767.0f);
        pcm[i * 2 + 1] = clamp_to_i16(sub_sample * 32767.0f);
    }
}

void vSample_task(void *args)
{
    configure_spi();
    initialize_pacer_timer(&sync_timer);

    if (spi_hdl == NULL || sync_timer == NULL) {
        finish_calibration_run(false);
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
        finish_calibration_run(false);
        vTaskDelete(NULL);
        return;
    }

    xEventGroupSync(sync_tasks, TASK_A_READY_BIT, ALL_TASKS_READY, portMAX_DELAY);

    ESP_ERROR_CHECK(gptimer_set_raw_count(sync_timer, 0));
    ESP_ERROR_CHECK(gptimer_start(sync_timer));

    uint64_t step_fp = (((uint64_t)PACER_TIMER_HZ) << 32) / SAMPLE_RATE;
    uint64_t next_tick_fp = step_fp;

    uint32_t t_start = esp_log_timestamp();
    for (int i = 0; i < N_SAMPLES; i++) {
        uint64_t target_tick = next_tick_fp >> 32;
        uint64_t now = 0;
        do {
            ESP_ERROR_CHECK(gptimer_get_raw_count(sync_timer, &now));
        } while (now < target_tick);
        next_tick_fp += step_fp;

        spi_device_polling_transmit(spi_hdl, &spi_t);
        uint32_t raw_res = ((uint32_t)spi_t.rx_data[0] << 16) |
                           ((uint32_t)spi_t.rx_data[1] <<  8) |
                            (uint32_t)spi_t.rx_data[2];
        samples[i] = (raw_res >> 2) & 0xFFFF;
    }
    uint32_t t_end = esp_log_timestamp();
    ESP_ERROR_CHECK(gptimer_stop(sync_timer));

    ESP_LOGI(SAMPLING_TAG, "Started: %u | Finished %u", t_start, t_end);
    float actual_fs = (float)N_SAMPLES / ((t_end - t_start) / 1000.0f);
    ESP_LOGI(SAMPLING_TAG, "Actual Sampling Frequency: %.2f Hz", actual_fs);

    float *band_gains = run_Auto_EQ_algorithm(samples, actual_fs);
    if (band_gains == NULL) {
        finish_calibration_run(false);
        vTaskDelete(NULL);
        return;
    }
    for (int i = 0; i < EQ_BANDS; ++i) {
        app_sliders[i] = band_gains[i];
    }

    activate_eq = true;
    flush_required = true;
    reset_filter_states();
    ble_publish_auto_eq_gains(app_sliders);

    free(band_gains);

    finish_calibration_run(true);
    vTaskDelete(NULL);
}

void vPlay_WAV_task(void* args)
{
    wav_playback_active = true;
    bool resume_bt_after_play = false;
    bool playback_failed = false;
    wave_reader_handle_t local_wav_hdl = NULL;

    // Suspend BM83 playback task whenever we take over I2S0 for WAV playback.
    if (is_task_suspendable(bt_task)) {
        if (mcu_rx != NULL) {
            i2s_channel_disable(mcu_rx);
        }
        vTaskSuspend(bt_task);
        resume_bt_after_play = !calibration_in_progress;
    }

    wave_header_t wav_head;
    local_wav_hdl = wave_reader_open("/storage/48k_4sec_sweep.wav");
    if (local_wav_hdl == NULL) {
        ESP_LOGE(WAV_TAG, "Unable to open WAV file");
        configure_i2s_for_audio(true);
        wav_playback_active = false;
        if (calibration_in_progress) {
            calibration_in_progress = false;
            ble_send_app_message("ERR:PLAY_WAV");
        }
        if (resume_bt_after_play && bt_task) {
            vTaskResume(bt_task);
        }
        vTaskDelete(NULL);
        return;
    }

    if (wave_read_header(local_wav_hdl, &wav_head) != 0) {
        ESP_LOGE(WAV_TAG, "Unable to read WAV file header");
        wave_reader_close(local_wav_hdl);
        configure_i2s_for_audio(true);
        wav_playback_active = false;
        if (calibration_in_progress) {
            calibration_in_progress = false;
            ble_send_app_message("ERR:PLAY_WAV");
        }
        if (resume_bt_after_play && bt_task) {
            vTaskResume(bt_task);
        }
        vTaskDelete(NULL);
        return;
    }

    print_wav(&wav_head);
    const uint32_t wav_sample_rate = (wav_head.sample_rate > 0) ? wav_head.sample_rate : SAMPLE_RATE;
    const uint32_t wav_channels = (wav_head.n_channels > 0) ? wav_head.n_channels : 1U;
    const uint32_t wav_bytes_per_sample = (wav_head.bytes_per_sample > 0) ? wav_head.bytes_per_sample : BYTES_PER_SAMPLE;

    if (wav_bytes_per_sample != 2U) {
        ESP_LOGE(WAV_TAG, "Unsupported WAV format: expected 16-bit PCM, got %lu bytes/sample",
                 (unsigned long)wav_bytes_per_sample);
        wave_reader_close(local_wav_hdl);
        configure_i2s_for_audio(true);
        wav_playback_active = false;
        if (calibration_in_progress) {
            calibration_in_progress = false;
            ble_send_app_message("ERR:PLAY_WAV");
        }
        if (resume_bt_after_play && bt_task) {
            vTaskResume(bt_task);
        }
        vTaskDelete(NULL);
        return;
    }

    const bool expand_mono_to_stereo = (wav_channels == 1U);
    const bool stereo_output = true;
    const size_t target_input_bytes = (size_t)((uint64_t)wav_head.samples_per_channel * wav_channels * wav_bytes_per_sample);
    const size_t target_output_bytes = expand_mono_to_stereo ? (target_input_bytes * 2U) : target_input_bytes;

    ESP_LOGI(WAV_TAG,
             "Playback path: file_channels=%lu, expand_mono_to_stereo=%s, target_in=%u bytes, target_out=%u bytes",
             (unsigned long)wav_channels,
             expand_mono_to_stereo ? "true" : "false",
             (unsigned)target_input_bytes,
             (unsigned)target_output_bytes);

    // Pre-load the full sweep into RAM before the sync point so the actual output starts much closer
    // to the ADC capture start.
    uint8_t *playback_buf = (uint8_t *)heap_caps_malloc(target_output_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (playback_buf == NULL) {
        ESP_LOGE(WAV_TAG, "Unable to allocate playback buffer (%u bytes)", (unsigned)target_output_bytes);
        wave_reader_close(local_wav_hdl);
        configure_i2s_for_audio(true);
        wav_playback_active = false;
        if (calibration_in_progress) {
            calibration_in_progress = false;
            ble_send_app_message("ERR:PLAY_WAV");
        }
        if (resume_bt_after_play && bt_task) {
            vTaskResume(bt_task);
        }
        vTaskDelete(NULL);
        return;
    }

    size_t total_input_bytes = 0;
    size_t total_output_bytes = 0;
    bool loaded_ok = load_and_expand_wav_to_buffer(local_wav_hdl,
                                                   wav_channels,
                                                   expand_mono_to_stereo,
                                                   target_input_bytes,
                                                   playback_buf,
                                                   target_output_bytes,
                                                   &total_input_bytes,
                                                   &total_output_bytes);
    wave_reader_close(local_wav_hdl);
    local_wav_hdl = NULL;

    if (!loaded_ok || total_input_bytes != target_input_bytes || total_output_bytes != target_output_bytes) {
        ESP_LOGE(WAV_TAG, "Failed to preload WAV data correctly: read=%u/%u wrote=%u/%u",
                 (unsigned)total_input_bytes,
                 (unsigned)target_input_bytes,
                 (unsigned)total_output_bytes,
                 (unsigned)target_output_bytes);
        free(playback_buf);
        configure_i2s_for_audio(true);
        wav_playback_active = false;
        if (calibration_in_progress) {
            calibration_in_progress = false;
            ble_send_app_message("ERR:PLAY_WAV");
        }
        if (resume_bt_after_play && bt_task) {
            vTaskResume(bt_task);
        }
        vTaskDelete(NULL);
        return;
    }

    refresh_filter_coeffs_if_needed(true);

    int16_t *pcm = (int16_t *)playback_buf;
    int total_samples = total_output_bytes / (sizeof(int16_t) * 2);
    process_stereo_pcm_inplace(pcm, total_samples);

    apply_volume_and_mute(pcm, total_output_bytes / sizeof(int16_t));

    configure_i2s_for_wav(wav_sample_rate, stereo_output);
    if (mcu_tx == NULL) {
        ESP_LOGE(WAV_TAG, "WAV I2S TX channel was not created");
        free(playback_buf);
        wav_playback_active = false;
        if (calibration_in_progress) {
            finish_calibration_run(false);
        } else if (resume_bt_after_play && bt_task) {
            vTaskResume(bt_task);
        }
        vTaskDelete(NULL);
        return;
    }

    if (calibration_in_progress && sync_tasks != NULL) {
        xEventGroupSync(sync_tasks, TASK_B_READY_BIT, ALL_TASKS_READY, portMAX_DELAY);
    }

    uint32_t t_start = esp_log_timestamp();
    size_t bytes_left = total_output_bytes;
    uint8_t *p = playback_buf;
    while (bytes_left > 0) {
        size_t wrote = 0;
        esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_left, &wrote, portMAX_DELAY);
        if (r != ESP_OK) {
            ESP_LOGE(WAV_TAG, "i2s_channel_write failed after %u ms, output=%u/%u bytes: %s",
                     (unsigned)(esp_log_timestamp() - t_start),
                     (unsigned)(total_output_bytes - bytes_left),
                     (unsigned)total_output_bytes,
                     esp_err_to_name(r));
            playback_failed = true;
            if (calibration_in_progress) {
                ble_send_app_message("CAL_FAILED");
                calibration_in_progress = false;
            }
            break;
        }

        if (wrote == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        bytes_left -= wrote;
        p += wrote;
    }
    uint32_t t_end = esp_log_timestamp();

    ESP_LOGI(WAV_TAG, "Started: %u | Finished %u | Read %u / %u bytes | Wrote %u / %u bytes",
             t_start,
             t_end,
             (unsigned)total_input_bytes,
             (unsigned)target_input_bytes,
             (unsigned)(total_output_bytes - bytes_left),
             (unsigned)target_output_bytes);

    free(playback_buf);

    configure_i2s_for_audio(true);

    wav_playback_active = false;
    if (resume_bt_after_play && bt_task) {
        vTaskResume(bt_task);
    }
    vTaskDelete(NULL);
}

void vUSB_playback_task(void *arg)
{
    configure_i2s_for_audio(false);
    usb_running = true;
    usb_uac_device_init();
    i2s_channel_enable(mcu_tx);

    static uint8_t silence[192] = {0};
    float processing_buffer_L[48];
    float processing_buffer_R[48];
    (void)processing_buffer_L;
    (void)processing_buffer_R;

    refresh_filter_coeffs_if_needed(true);

    while (1) {
        usb_running = true;
        refresh_filter_coeffs_if_needed(false);
        if (flush_required) {
            flush_required = false;

            size_t bytes_received = 0;
            uint8_t *stale;
            do {
                stale = (uint8_t *)xRingbufferReceiveUpTo(audio_ringbuf, &bytes_received, 0, 192 * 100);
                if (stale) vRingbufferReturnItem(audio_ringbuf, stale);
            } while (stale);

            size_t written;
            for (int i = 0; i < 8; i++) {
                i2s_channel_write(mcu_tx, silence, sizeof(silence), &written, pdMS_TO_TICKS(10));
            }
            continue;
        }

        size_t bytes_received = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(audio_ringbuf, &bytes_received, portMAX_DELAY, 192);

        if (data) {
            int16_t *pcm_in = (int16_t *)data;
            int num_samples = bytes_received / (sizeof(int16_t) * 2);

            process_stereo_pcm_inplace(pcm_in, num_samples);
            apply_volume_and_mute((int16_t *)data, bytes_received / sizeof(int16_t));
            size_t bytes_written = 0;
            i2s_channel_write(mcu_tx, data, bytes_received, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(audio_ringbuf, data);
        }
        usb_running = false;
    }
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
    configure_i2s_for_audio(true);
    ESP_LOGI(BM83_TAG, "I2S Configured");

    uint8_t *bt_buff = (uint8_t *)heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(bt_buff);

    size_t bytes_read;
    size_t wrote = 0;

    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_INPUT);
    gpio_pullup_dis(GPIO_NUM_2);
    gpio_pulldown_dis(GPIO_NUM_2);
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_INPUT);

    esp_rom_gpio_connect_out_signal(GPIO_NUM_2, 0x100, false, false);
    esp_rom_gpio_connect_in_signal(GPIO_NUM_2, 25, false);

    gpio_set_drive_capability(I2S_BIT_CLK, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(I2S_LRCLK_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(I2S_TX_LINE, GPIO_DRIVE_CAP_0);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int num_samples = BUFFER_BYTES / 2;
    float *float_conv_buff = (float *)heap_caps_malloc(num_samples * sizeof(float), MALLOC_CAP_INTERNAL);
    (void)float_conv_buff;

    refresh_filter_coeffs_if_needed(true);

    while (1)
    {
        if (mcu_rx == NULL || mcu_tx == NULL) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        esp_err_t r = i2s_channel_read(mcu_rx, bt_buff, BUFFER_BYTES, &bytes_read, portMAX_DELAY);
        if (r != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        refresh_filter_coeffs_if_needed(false);
        process_stereo_pcm_inplace((int16_t *)bt_buff, bytes_read / FRAME_SIZE_BYTES);
        apply_volume_and_mute((int16_t *)bt_buff, bytes_read / sizeof(int16_t));

        size_t bytes_to_w = bytes_read;
        uint8_t *p = bt_buff;

        while (bytes_to_w > 0)
        {
            ESP_ERROR_CHECK(i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY));
            bytes_to_w -= wrote;
            p += wrote;
        }
    }

    free(bt_buff);
    i2s_channel_disable(mcu_tx);
    i2s_channel_disable(mcu_rx);
    vTaskDelete(NULL);
}
