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

    float* fir_taps = run_Auto_EQ_algorithm(samples, actual_fs);

	activate_eq = true;
	vTaskResume(bt_task);	// Resume once FIR coefficients are calculated

    vTaskDelete(NULL);  // Delete the task when done
}

void vPlay_WAV_task(void* args)
{	
	// Check status of BT module and USB (if busy, suspend)
	eTaskState bt_state  = eTaskGetState(bt_task);
	// eTaskState usb_state = eTaskGetState(usb_task);

	if (bt_state != NULL && bt_state == eRunning)
	{
		i2s_channel_disable(mcu_rx);
		i2s_channel_disable(mcu_tx);
		vTaskSuspend(bt_task);
	} //else if (usb_state != NULL)	// && usb_state == eRunning
	// {
	// 	i2s_channel_disable(mcu_tx);
	// 	vTaskSuspend(usb_task);
	// }

	configure_i2s_for_wav();

	if (mcu_rx != NULL) {
		i2s_channel_disable(mcu_rx);
	}

	wave_header_t wav_head;
	wav_hdl = wave_reader_open("/storage/stereo_sweep.wav");
			
	if (wav_hdl == NULL) {
		ESP_LOGE(WAV_TAG, "Unable to open read!");
		vTaskDelete(NULL);
	}
		
	if (wave_read_header(wav_hdl, &wav_head) != 0) {
		ESP_LOGE(WAV_TAG, "Unable to read WAV file header!");
		vTaskDelete(NULL);
	}
		
	uint8_t *buff = (uint8_t *)heap_caps_malloc(BUFFER_BYTES * 2 * sizeof(uint8_t), MALLOC_CAP_DEFAULT);	// Allocate space to store data coming from BT module
	assert(buff);	
	size_t wrote, pos = 0;

	ESP_ERROR_CHECK(i2s_channel_enable(mcu_tx));	// Enable I2S channel for transmission

	xEventGroupSync(
        sync_tasks,
        TASK_B_READY_BIT,
        ALL_TASKS_READY,
        portMAX_DELAY
    );

	uint32_t t_start = esp_log_timestamp();
	while (1)
	{
		size_t bytes_read = wave_read_raw_data(wav_hdl, buff, pos, BUFFER_BYTES);		// Read from WAV file
			
		if (bytes_read == 0){
			break;
		}
			
		pos += bytes_read;
		size_t bytes_to_w = bytes_read;
		uint8_t *p = buff;
			
		while (bytes_to_w > 0) {
			wrote = 0;
			esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY);
			bytes_to_w -= wrote;
			p += wrote;
		}// end of inner while loop
			
	}// end of main while loop 
	uint32_t t_end = esp_log_timestamp();

	ESP_LOGI(WAV_TAG, "Started: %u | Finished %u", t_start, t_end);
		
	i2s_channel_disable(mcu_tx);
	i2s_del_channel(mcu_tx);
	wave_reader_close(wav_hdl);	// close wav file

	xEventGroupSetBits(sync_tasks, TASK_B_DONE_BIT);
	
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
		/*if (bytes_read > 0) {
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
		}*/
		
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