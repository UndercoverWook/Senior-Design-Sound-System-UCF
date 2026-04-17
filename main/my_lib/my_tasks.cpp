#include "my_tasks.h"
#include "glb_params.h"
#include "help_functions.h"
#include "auto_eq_help.h"
#include "config_functions.h"
#include "my_usb_device.h"


void vSample_task(void *args)
{
	configure_spi();

    spi_transaction_t spi_t {
		.flags 		= SPI_TRANS_USE_RXDATA,
        .length     = TRANSACTION_LENGTH,
        .rxlength   = TRANSACTION_LENGTH,
    };

    uint16_t *samples = (uint16_t *)heap_caps_malloc(N_SAMPLES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);  // Sample Data array
    assert(samples);

	xEventGroupSync(
        sync_tasks,
        TASK_A_READY_BIT,   // bit this task sets
        ALL_TASKS_READY,    // bits to wait for
        portMAX_DELAY
    );

	spi_device_acquire_bus(spi_hdl, portMAX_DELAY);

    uint32_t t_start = esp_log_timestamp();
    for (int i = 0; i < N_SAMPLES; i++) {
        spi_device_polling_transmit(spi_hdl, &spi_t);  // Transmit the SPI transaction
        uint32_t raw_res = ((uint32_t)spi_t.rx_data[0] << 16) | 
                           ((uint32_t)spi_t.rx_data[1] <<  8) | 
                            (uint32_t)spi_t.rx_data[2];
        samples[i] = (raw_res >> 2) & 0xFFFF;  // Store only the lower 16 bits (the actual ADC value)
    }
    uint32_t t_end = esp_log_timestamp();

	spi_device_release_bus(spi_hdl);
	
	ESP_LOGI(SAMPLING_TAG, "Started: %u | Finished %u", t_start, t_end);

	// Calculate actual sampling frequency
    float actual_fs = (float)N_SAMPLES / ((t_end - t_start) / 1000.0f);
	ESP_LOGI(SAMPLING_TAG, "Actual Sampling Frequency: %.2f Hz", actual_fs);

	// Wait for both tasks to finish before runnning FFT
	xEventGroupSetBits(sync_tasks, TASK_A_DONE_BIT);
	xEventGroupWaitBits(sync_tasks, ALL_TASKS_DONE, pdFALSE, pdTRUE, portMAX_DELAY);

    //float* fir_taps = run_Auto_EQ_algorithm(samples, actual_fs);

	activate_eq = true;
	// vTaskResume(usb_task);	// Resume once FIR coefficients are calculated

    vTaskDelete(NULL);  // Delete the task when done
}

void vPlay_WAV_task(void* args)
{
    // Open and read header first
    wave_reader_handle_t wav_f = wave_reader_open("/storage/48k_4sec_sweep.wav");
    if (wav_f == NULL) {
        ESP_LOGE(WAV_TAG, "Unable to open wav file");
        vTaskDelete(NULL);
    }

    wave_header_t wav_head;
    if (wave_read_header(wav_f, &wav_head) != 0) {
        ESP_LOGE(WAV_TAG, "Unable to read WAV header");
        wave_reader_close(wav_f);
        vTaskDelete(NULL);
    }

    uint32_t pcm_size   = N_SAMPLES * TEST_DURATION * CHANNELS;
    uint8_t *wav_samples = (uint8_t *)heap_caps_malloc(pcm_size, MALLOC_CAP_SPIRAM);
    assert(wav_samples);

    uint8_t *buff = (uint8_t *)calloc(1, BUFFER_BYTES);
    assert(buff);

    // Load entire file into SPIRAM
    size_t pos = 0;
    while (pos < pcm_size) {
        size_t bytes_read = wave_read_raw_data(wav_f, buff, pos, BUFFER_BYTES);
        if (bytes_read == 0) break;
        memcpy(wav_samples + pos, buff, bytes_read);
        pos += bytes_read;
    }
    size_t total_loaded = pos;
    ESP_LOGI(WAV_TAG, "Loaded %u bytes into SPIRAM", total_loaded);

    wave_reader_close(wav_f);  // closed once, here
    free(buff);

    // Suspend USB if running
    eTaskState usb_state = eTaskGetState(usb_task);
    if (usb_state == eRunning) {
        i2s_channel_disable(mcu_tx);
        vTaskSuspend(usb_task);
    }

    ESP_ERROR_CHECK(i2s_channel_enable(mcu_tx));

    xEventGroupSync(
        sync_tasks, 
        TASK_B_READY_BIT,
         ALL_TASKS_READY, 
         portMAX_DELAY
    );

    uint8_t *p          = wav_samples;
    size_t   bytes_to_w = total_loaded;
    size_t   wrote      = 0;

    uint32_t t_start = esp_log_timestamp();
    while (bytes_to_w > 0) {
        wrote = 0;
        esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY);
        if (r != ESP_OK) break;
        bytes_to_w -= wrote;
        p          += wrote;
    }
    uint32_t t_end = esp_log_timestamp();

    ESP_LOGI(WAV_TAG, "Started: %u | Finished: %u", t_start, t_end);

    i2s_channel_disable(mcu_tx);
    i2s_del_channel(mcu_tx);
    free(wav_samples);

    xEventGroupSetBits(sync_tasks, TASK_B_DONE_BIT);
    vTaskDelete(NULL);
}

void vUSB_playback_task(void *arg)
{
    usb_uac_device_init();
    i2s_channel_enable(mcu_tx);

    static uint8_t silence[192] = {0};
    float processing_buffer_L[48];      // Process channels differently
    float processing_buffer_R[48];

    float crossover_f = 100.0f; 

    // Generate Crossover (100Hz Butterworth)
    dsps_biquad_gen_lpf_f32(lpf_coeffs, crossover_f / SAMPLE_RATE, 0.707f);
    dsps_biquad_gen_hpf_f32(hpf_coeffs, crossover_f / SAMPLE_RATE, 0.707f);

    // Generate the 8 EQ Stages (Peaking EQ)
    for (int i = 0; i < 8; i++) {
        float gain = app_sliders[i];
        float Q = 1.0f;
        dsps_biquad_gen_lowShelf_f32(eq_coeffs[i], eq_freqs[i] / SAMPLE_RATE, gain, Q);
    }

    while (1) {
        // Handle flush before pulling new data
        if (flush_required) {
            flush_required = false;

            // Drain ring buffer first
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
            continue;  // loop back and wait for fresh data
        }

        size_t bytes_received = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(audio_ringbuf, &bytes_received, portMAX_DELAY, 192);

        if (data) {
            int16_t *pcm_in = (int16_t *)data;
            int num_samples = bytes_received / (sizeof(int16_t) * 2); // 48 samples

            for (int i = 0; i < num_samples; i++) {
                // Convert to float for IIR Biquad
                float left_in = (float)pcm_in[i * 2] / 32768.0f;
                float right_in = (float)pcm_in[i * 2 + 1] / 32768.0f;
                float mono_sample = (left_in + right_in) * 0.4f;        // May need to adjust last number for clipping

                // Run through the 8-band EQ cascade
                float eq_sample = mono_sample;
                for (int b = 0; b < 8; b++) {
                    float out;
                    dsps_biquad_f32_aes3(&eq_sample, &out, 1, eq_coeffs[b], eq_w[b]);
                    eq_sample = out;
                }

                // Crossover Filtering (Left to Sub, Right to M&H)
                float sub_sample, mid_sample;
                dsps_biquad_f32_aes3(&eq_sample, &sub_sample, 1, lpf_coeffs, sub_lpf_w);
                dsps_biquad_f32_aes3(&eq_sample, &mid_sample, 1, hpf_coeffs, mid_hpf_w);

                // Back to 16-bit data
                pcm_in[i * 2]     = (int16_t)(sub_sample * 32767.0f);
                pcm_in[i * 2 + 1] = (int16_t)(mid_sample * 32767.0f);
                apply_volume_and_mute((int16_t *)data, bytes_received / sizeof(int16_t));
                size_t bytes_written = 0;
                i2s_channel_write(mcu_tx, data, bytes_received, &bytes_written, portMAX_DELAY);
                vRingbufferReturnItem(audio_ringbuf, data);
            }

        size_t bytes_written = 0;
        i2s_channel_write(mcu_tx, data, bytes_received, &bytes_written, portMAX_DELAY);
        vRingbufferReturnItem(audio_ringbuf, data);
    }

    vTaskDelete(NULL);
}


// if (data) {
        //     apply_volume_and_mute((int16_t *)data, bytes_received / sizeof(int16_t));
        //     size_t bytes_written = 0;
        //     i2s_channel_write(mcu_tx, data, bytes_received, &bytes_written, portMAX_DELAY);
        //     vRingbufferReturnItem(audio_ringbuf, data);
        // }





/*
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
	//init_eq(48000.0f);

	uint8_t *bt_buff = (uint8_t *)calloc(1, BUFFER_BYTES);	// Initialize array to store data coming from BT module
	assert(bt_buff);	
	
	size_t bytes_read;
	size_t wrote = 0;

	esp_rom_gpio_connect_out_signal(GPIO_NUM_2, 0x100, false, false);
    esp_rom_gpio_connect_in_signal(GPIO_NUM_2, 25, false);

    gpio_set_drive_capability(I2S_BIT_CLK, GPIO_DRIVE_CAP_0); // Lowest drive
    gpio_set_drive_capability(I2S_LRCLK_PIN, GPIO_DRIVE_CAP_0);
   	gpio_set_drive_capability(I2S_TX_LINE, GPIO_DRIVE_CAP_0);

	gpio_config_t io_conf = {
		.pin_bit_mask = (1ULL << GPIO_NUM_2),
		.mode 		  = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE,
	};
	gpio_config(&io_conf);

	//float *left_float = (float *)heap_caps_aligned_alloc(16, (BUFFER_BYTES / 4) * sizeof(float), MALLOC_CAP_INTERNAL);
	//float *right_float = (float *)heap_caps_aligned_alloc(16, (BUFFER_BYTES / 4) * sizeof(float), MALLOC_CAP_INTERNAL);

	// Read bytes from the Bluetooth Module (MCU acts as Receiver) and echo/send it to the DAC (MCU acts as Sender)
	while (1)
	{
		esp_err_t r = i2s_channel_read(mcu_rx, bt_buff, BUFFER_BYTES, &bytes_read, portMAX_DELAY);
		//ESP_LOGI("BM83", "Read %d bytes", bytes_read);
		
		size_t bytes_to_w = bytes_read;
		uint8_t *p = bt_buff;
		if (bytes_read > 0) {
			int16_t *raw_samples = (int16_t *)bt_buff;
			int samples_per_channel = bytes_read / 4;

			for (int i = 0; i < samples_per_channel; i++) {
				left_float[i] = ((float)raw_samples[i * 2] / 32768.0f) * 0.5f;
				right_float[i] = ((float)raw_samples[i * 2 + 1] / 32768.0f) * 0.5f;
			}

			for (int b = 0; b < EQ_BANDS; b++) {
				// Left Channel
				dsps_biquad_f32_ae32(left_float, left_float, samples_per_channel, &eq_coeffs[b * 5], &delay_l[b * 2]);
				// Right Channel
				dsps_biquad_f32_ae32(right_float, right_float, samples_per_channel, &eq_coeffs[b * 5], &delay_r[b * 2]);
			}

			for (int i = 0; i < samples_per_channel; i++) {
				float l = left_float[i] * 32767.0f;
				float r = right_float[i] * 32767.0f;
				
				raw_samples[i * 2] = (int16_t)((l > 32767) ? 32767 : (l < -32768) ? -32768 : l);
				raw_samples[i * 2 + 1] = (int16_t)((r > 32767) ? 32767 : (r < -32768) ? -32768 : r);
			}
		}
		
		while (bytes_to_w > 0)
		{			
			ESP_ERROR_CHECK(i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY));
			
			if (wrote == 0) 
			{ 
				vTaskDelay(pdMS_TO_TICKS(1)); 
				continue; 
			}	
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
*/