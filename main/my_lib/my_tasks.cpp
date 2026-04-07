#include "my_tasks.h"
#include "glb_params.h"
#include "help_functions.h"

void vSample_task(void *args) 
{
    spi_transaction_t spi_t {
        .flags      = SPI_TRANS_USE_RXDATA,
        .length     = TRANSACTION_LENGTH,
        .rxlength   = TRANSACTION_LENGTH,
    };

    uint16_t *samples = (uint16_t *)heap_caps_malloc(N_SAMPLES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);  // Sample Data array
    
    uint32_t t_start = esp_log_timestamp();
    for (int i = 0; i < N_SAMPLES; i++) {
        spi_device_polling_transmit(spi_hdl, &spi_t);  // Transmit the SPI transaction
        uint32_t raw_res = ((uint32_t)spi_t.rx_data[0] << 16) | 
                           ((uint32_t)spi_t.rx_data[1] <<  8) | 
                            (uint32_t)spi_t.rx_data[2];
        samples[i] = (raw_res >> 2) & 0xFFFF;  // Store only the lower 16 bits (the actual ADC value)
    }
    uint32_t t_end = esp_log_timestamp();
    float actual_fs = (float)N_SAMPLES / ((t_end - t_start) / 1000.0f);  // Calculate actual sampling frequency
    ESP_LOGI(SAMPLING_TAG, "Actual Sampling Frequency: %.2f Hz", actual_fs);

    // Initialize FFT tables (must be done before calling any FFT functions) and compute FFT
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE("FFT", "Failed to initialize FFT, ERROR: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(SAMPLING_TAG, "Running FFT on sampled data...");
    //run_EQ_Algorithm(samples, &actual_fs);

    vTaskDelete(NULL);  // Delete the task when done
}

void vPlay_WAV_Task1(void* args)
{	
	wave_header_t wav_head;
	wav_hdl = wave_reader_open("/storage/44kh_full_sweep.wav");
			
	if (wav_hdl == NULL) {
		ESP_LOGE(WAV_FILE, "Unable to open read!");
		return;
	}
		
	if (wave_read_header(wav_hdl, &wav_head) == 0) {
		print_wav(&wav_head);
	} else {
		ESP_LOGE(WAV_FILE, "Unable to read WAV file header!");
		return;
	}
		
	uint8_t *buff = (uint8_t *)calloc(1, BUFFER_BYTES);	// Allocate space to store data coming from BT module
	assert(buff);	
	size_t wrote, pos = 0;
	
	i2s_channel_enable(mcu_tx);	// Enable I2S channel for transmission

	while (1)
	{
		//i2s_channel_read(mcu_rx, buff, BUFFER_BYTES, &bytes_read, portMAX_DELAY);		// Read data from BM83
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
				
			if (r != ESP_OK){
				break;
			}
				
			if (wrote == 0) {
				vTaskDelay(pdMS_TO_TICKS(1));
				continue;
			}
			bytes_to_w -= wrote;
			p += wrote;
		}// end of inner while loop
			
	}// end of main while loop 
		
	i2s_channel_disable(mcu_tx);
	wave_reader_close(wav_hdl);	// close wav file
	free(buff);
	vTaskDelete(NULL);
}

void vUSB_playback_task(void *arg)
{
	i2s_channel_enable(mcu_tx);	// Enable I2S channel for transmission

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
	uint8_t *bt_buff = (uint8_t *)calloc(1, BUFFER_BYTES);	// Initialize array to store data coming from BT module
	assert(bt_buff);	
	
	size_t bytes_read;
	size_t wrote = 0;

	i2s_channel_enable(mcu_tx);	// Enable I2S channel for transmission (to DAC)
	i2s_channel_enable(mcu_rx);	// Enable I2S channel for reception (from BT module)
	
	// Read bytes from the Bluetooth Module (MCU acts as Receiver) and echo/send it to the DAC (MCU acts as Sender)
	while (1)
	{
		i2s_channel_read(mcu_rx, bt_buff, BUFFER_BYTES, &bytes_read, portMAX_DELAY);
		
		size_t bytes_to_w = bytes_read;
		uint8_t *p = bt_buff;
		
		while (bytes_to_w > 0)
		{			
			//ESP_LOGI(TAG, "Bytes to write: [%d]", (unsigned)bytes_to_w);
			esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY);
	
			if (r != ESP_OK){
				break;
			}
			
			if (wrote == 0)
			{
				vTaskDelay(pdMS_TO_TICKS(1));
				continue;
			}
			bytes_to_w -= wrote;		// If written -> OK, then decrease counter
			p += wrote;					// Increase pointer to buffer			
		}// end of inner while loop
	}// end of main while loop
	
	free(bt_buff);
	i2s_channel_disable(mcu_tx);
	i2s_channel_disable(mcu_rx);
	vTaskDelete(NULL);
}