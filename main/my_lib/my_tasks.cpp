#include "my_tasks.h"
#include "glb_params.h"
#include "help_functions.h"
#include "auto_eq_help.h"
#include "config_functions.h"
#include "my_usb_device.h"
#include "ble_control.h"


void vSample_task(void *args)
{
	configure_spi();

    spi_transaction_t spi_t {
        .flags      = SPI_TRANS_USE_RXDATA,
        .length     = TRANSACTION_LENGTH,
        .rxlength   = TRANSACTION_LENGTH,
    };

    uint16_t *samples = (uint16_t *)heap_caps_malloc(N_SAMPLES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);  // Sample Data array
    
	xEventGroupSync(
        sync_tasks,
        TASK_A_READY_BIT,   // bit this task sets
        ALL_TASKS_READY,    // bits to wait for
        portMAX_DELAY
    );

    uint32_t t_start = esp_log_timestamp();
    for (int i = 0; i < N_SAMPLES; i++) {
        spi_device_polling_transmit(spi_hdl, &spi_t);  // Transmit the SPI transaction
        uint32_t raw_res = ((uint32_t)spi_t.rx_data[0] << 16) | 
                           ((uint32_t)spi_t.rx_data[1] <<  8) | 
                            (uint32_t)spi_t.rx_data[2];
        samples[i] = (raw_res >> 2) & 0xFFFF;  // Store only the lower 16 bits (the actual ADC value)
    }
    uint32_t t_end = esp_log_timestamp();
	
	ESP_LOGI(SAMPLING_TAG, "Started: %u | Finished %u", t_start, t_end);

	// Calculate actual sampling frequency
    float actual_fs = (float)N_SAMPLES / ((t_end - t_start) / 1000.0f);
	ESP_LOGI(SAMPLING_TAG, "Actual Sampling Frequency: %.2f Hz", actual_fs);

    float* fir_taps = run_Auto_EQ_algorithm(samples, actual_fs);
    (void)fir_taps;

    calibration_in_progress = false;
    ble_send_app_message("CAL_DONE");
	vTaskResume(bt_task);	// Resume once FIR coefficients are calculated

    vTaskDelete(NULL);  // Delete the task when done
}

void vPlay_WAV_task(void* args)
{	
    wav_playback_active = true;
    bool resume_bt_after_play = false;
    bool playback_failed = false;
    wave_reader_handle_t local_wav_hdl = NULL;

	// Check status of BT module (if busy, suspend)
	eTaskState bt_state  = eTaskGetState(bt_task);
	//eTaskState usb_state = eTaskGetState(usb_task);

	if (bt_state == eRunning)
	{
        if (mcu_rx != NULL) {
            i2s_channel_disable(mcu_rx);
        }
		vTaskSuspend(bt_task);
        resume_bt_after_play = !calibration_in_progress;
	}// else if (usb_state == eRunning)
	// {
	// 	i2s_channel_disable(mcu_tx);
	// 	vTaskSuspend(usb_task);
	// }

	wave_header_t wav_head;
	local_wav_hdl = wave_reader_open("/storage/44k_full_sweep.wav");
			
	if (local_wav_hdl == NULL) {
		ESP_LOGE(WAV_TAG, "Unable to open WAV file");
        configure_i2s_for_audio(true);
        wav_playback_active = false;
        if (calibration_in_progress) {
            calibration_in_progress = false;
            ble_send_app_message("ERR:PLAY_WAV");
        }
        if (resume_bt_after_play) {
            vTaskResume(bt_task);
        }
		vTaskDelete(NULL);
        return;
	}
		
	if (wave_read_header(local_wav_hdl, &wav_head) != 0) {
		ESP_LOGE(WAV_TAG, "Unable to read WAV file header");
        if (local_wav_hdl != NULL) {
            wave_reader_close(local_wav_hdl);
            local_wav_hdl = NULL;
        }
        configure_i2s_for_audio(true);
        wav_playback_active = false;
        if (calibration_in_progress) {
            calibration_in_progress = false;
            ble_send_app_message("ERR:PLAY_WAV");
        }
        if (resume_bt_after_play) {
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
        if (local_wav_hdl != NULL) {
            wave_reader_close(local_wav_hdl);
            local_wav_hdl = NULL;
        }
        configure_i2s_for_audio(true);
        wav_playback_active = false;
        if (calibration_in_progress) {
            calibration_in_progress = false;
            ble_send_app_message("ERR:PLAY_WAV");
        }
        if (resume_bt_after_play) {
            vTaskResume(bt_task);
        }
        vTaskDelete(NULL);
        return;
    }

    const bool expand_mono_to_stereo = (wav_channels == 1U);
    const bool stereo_output = true;
    const size_t target_input_bytes = (size_t)((uint64_t)wav_sample_rate * TEST_DURATION * wav_channels * wav_bytes_per_sample);
    const size_t target_output_bytes = expand_mono_to_stereo ? (target_input_bytes * 2U) : target_input_bytes;
    const size_t max_input_chunk = expand_mono_to_stereo ? (BUFFER_BYTES / 2U) : BUFFER_BYTES;

    ESP_LOGI(WAV_TAG,
             "Playback path: file_channels=%lu, expand_mono_to_stereo=%s, target_in=%u bytes, target_out=%u bytes",
             (unsigned long)wav_channels,
             expand_mono_to_stereo ? "true" : "false",
             (unsigned)target_input_bytes,
             (unsigned)target_output_bytes);

    configure_i2s_for_wav(wav_sample_rate, stereo_output);
		
	uint8_t *file_buff = (uint8_t *)calloc(1, BUFFER_BYTES);
	uint8_t *tx_buff = (uint8_t *)calloc(1, BUFFER_BYTES);
	assert(file_buff);	
	assert(tx_buff);
	
    size_t pos = 0;
    size_t total_input_bytes = 0;
    size_t total_output_bytes = 0;

    if (calibration_in_progress && sync_tasks != NULL) {
	xEventGroupSync(
        sync_tasks,
        TASK_B_READY_BIT,
        ALL_TASKS_READY,
        portMAX_DELAY
    );
    }

	uint32_t t_start = esp_log_timestamp();
	while (total_input_bytes < target_input_bytes)
	{
        size_t input_left = target_input_bytes - total_input_bytes;
        size_t read_size = (input_left < max_input_chunk) ? input_left : max_input_chunk;
		size_t bytes_read = wave_read_raw_data(local_wav_hdl, file_buff, pos, read_size);
			
		if (bytes_read == 0){
			break;
		}
			
		pos += bytes_read;
        total_input_bytes += bytes_read;

        uint8_t *write_ptr = file_buff;
        size_t write_size = bytes_read;

        if (expand_mono_to_stereo) {
            size_t sample_count = bytes_read / sizeof(int16_t);
            int16_t *src = (int16_t *)file_buff;
            int16_t *dst = (int16_t *)tx_buff;

            for (size_t i = 0; i < sample_count; ++i) {
                dst[(i * 2U) + 0U] = src[i];
                dst[(i * 2U) + 1U] = src[i];
            }

            write_ptr = tx_buff;
            write_size = sample_count * FRAME_SIZE_BYTES;
        }

		size_t bytes_to_w = write_size;
		uint8_t *p = write_ptr;
			
		while (bytes_to_w > 0) {
			size_t wrote = 0;
			esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY);

            if (r != ESP_OK){
                ESP_LOGE(WAV_TAG, "i2s_channel_write failed after %u ms, input=%u bytes, output=%u bytes: %s",
                         (unsigned)(esp_log_timestamp() - t_start),
                         (unsigned)total_input_bytes,
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
			bytes_to_w -= wrote;
			p += wrote;
            total_output_bytes += wrote;
		}// end of inner while loop

        if (playback_failed) {
            break;
        }
			
	}// end of main while loop 
	uint32_t t_end = esp_log_timestamp();

	ESP_LOGI(WAV_TAG, "Started: %u | Finished %u | Read %u / %u bytes | Wrote %u / %u bytes",
             t_start,
             t_end,
             (unsigned)total_input_bytes,
             (unsigned)target_input_bytes,
             (unsigned)total_output_bytes,
             (unsigned)target_output_bytes);
		
	if (local_wav_hdl != NULL) {
        wave_reader_close(local_wav_hdl);
        local_wav_hdl = NULL;
    }	// close wav file
	free(file_buff);
    free(tx_buff);

    configure_i2s_for_audio(true);

    wav_playback_active = false;
    if (resume_bt_after_play) {
        vTaskResume(bt_task);
    }
	vTaskDelete(NULL);
}


void vUSB_playback_task(void *arg)
{
	configure_i2s_for_audio(false);	// Set bluetooth == false
    usb_uac_device_init();			// Initialize USB UAC device class

	// gpio_set_drive_capability(I2S_BIT_CLK, GPIO_DRIVE_CAP_0); // Lowest drive
    // gpio_set_drive_capability(I2S_LRCLK_PIN, GPIO_DRIVE_CAP_0);
    // gpio_set_drive_capability(I2S_TX_LINE, GPIO_DRIVE_CAP_0);

    while (1) {
        size_t bytes_received = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(audio_ringbuf, &bytes_received, portMAX_DELAY, 192);
        if (data) {
            size_t bytes_written = 0;
            i2s_channel_write(mcu_tx, data, bytes_received, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(audio_ringbuf, data);
        }
    }
	vTaskDelete(NULL);
}

void vBT_playback_task(void *arg)
{
	bm83_tx_ind_init();
	// Wait for BT inidication of Paired Device
	while (1) {
        int level = gpio_get_level(MCU_WAKE);
		if (level == 0) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

	ESP_LOGI(BM83_TAG, "BM83 Transmitting!");
	configure_i2s_for_audio(true);
	ESP_LOGI(BM83_TAG, "I2S Configured");

	uint8_t *bt_buff = (uint8_t *)heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);	// Initialize array to store data coming from BT module
	assert(bt_buff);	
	
	size_t bytes_read;
	size_t wrote = 0;

	gpio_set_direction(GPIO_NUM_2, GPIO_MODE_INPUT);
    gpio_pullup_dis(GPIO_NUM_2);
    gpio_pulldown_dis(GPIO_NUM_2);
	gpio_set_direction(GPIO_NUM_2, GPIO_MODE_INPUT);

    esp_rom_gpio_connect_out_signal(GPIO_NUM_2, 0x100, false, false);
    esp_rom_gpio_connect_in_signal(GPIO_NUM_2, 25, false);

    gpio_set_drive_capability(I2S_BIT_CLK, GPIO_DRIVE_CAP_0); // Lowest drive
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
		
	// Read bytes from the Bluetooth Module (MCU acts as Receiver) and echo/send it to the DAC (MCU acts as Sender)
	while (1)
	{
		esp_err_t r = i2s_channel_read(mcu_rx, bt_buff, BUFFER_BYTES, &bytes_read, portMAX_DELAY);

		// Process 16-bit data to float for FIR processing (convert back to 16-bit for DAC)
		// if (activate_eq && bytes_read > 0) {
		// 	int16_t *raw_samples = (int16_t *)bt_buff;
		// 	int sample_count = bytes_read / 2;

		// 	for (int i = 0; i < sample_count; i++) {
		// 		float_conv_buff[i] = (raw_samples[i] / 32768.0f) * 0.25f;
		// 	}

		// 	dsps_fir_f32_aes3(&global_eq, float_conv_buff, float_conv_buff, sample_count);

		// 	for (int i = 0; i < sample_count; i++) {
		// 		float val = float_conv_buff[i] * 32768.0f;
		// 		if (val > 32767.0f) val = 32767.0f;
		// 		if (val < -32768.0f) val = -32768.0f;
		// 		raw_samples[i] = (int16_t)val;
		// 	}
		// }
		
		size_t bytes_to_w = bytes_read;
		uint8_t *p = bt_buff;
		
		while (bytes_to_w > 0)
		{			
			ESP_ERROR_CHECK(i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY));
			bytes_to_w -= wrote;		// If written -> OK, then decrease counter
			p += wrote;					// Increase pointer to buffer			
		}// end of inner while loop
	}// end of main while loop

	ESP_LOGW(BM83_TAG, "Bluetooth device disconnected");
	
	free(bt_buff);
	i2s_channel_disable(mcu_tx);
	i2s_channel_disable(mcu_rx);
	vTaskDelete(NULL);
}