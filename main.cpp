#include "driver/uart.h"
#include "hal/uart_types.h"
#include "soc/clk_tree_defs.h"
#include <cstdio>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
	#include "driver/i2s_common.h"
	#include "driver/i2s_types.h"
	#include "driver/spi_common.h"
	#include "driver/spi_master.h"
	#include "hal/spi_types.h"
	#include "esp_log.h"
	#include "driver/i2s_std.h"
	#include "freertos/idf_additions.h"
	#include "freertos/projdefs.h"
	#include "hal/i2s_types.h"
	#include "portmacro.h"
	#include "soc/gpio_num.h"
	#include "esp_spiffs.h"
	#include "driver/gpio.h"
	#include "freertos/FreeRTOS.h"
	#include "freertos/task.h"
	#include "wave.h"
	#include "wave_common.h"
	#include "wave_reader.h"
}

static const char *TAG = "I2S_DEMO";
static const char *WAV_FILE = "WAV Test";
static i2s_chan_handle_t mcu_rx = NULL;
static i2s_chan_handle_t mcu_tx = NULL;
static wave_reader_handle_t wav_hdl = NULL;
static const gpio_num_t I2S_DIN_LINE  = GPIO_NUM_2;		// Data out from BM83 to MCU through I2S
static const gpio_num_t I2S_LRCLK_PIN = GPIO_NUM_4;		// LRCLK line connected to the BM83 and DAC
static const gpio_num_t I2S_BCLK_PIN  = GPIO_NUM_5;		// Bit Clock line connected to the BM83 and DAC
static const gpio_num_t I2S_DOUT_LINE = GPIO_NUM_6;		// Data out from MCU to DAC through I2S
static const gpio_num_t BT_WAKE		  = GPIO_NUM_48;	// Connected to MFB pin from BM83 to put it in pairing mode


#define SAMPLE_RATE      44100									// Sampling Rate (Matched with BM83)
#define BUFFER_FRAMES  	 1024            						// frames (stereo frames) captured and transmitted 
#define BYTES_PER_SAMPLE 2               						// 16-bit => 2 bytes per channel sample
#define CHANNELS         2										// We are using Left & Right
#define FRAME_SIZE_BYTES (BYTES_PER_SAMPLE * CHANNELS)   		// 4 bytes per frame (16-bit stereo)
#define BUFFER_BYTES     (BUFFER_FRAMES * FRAME_SIZE_BYTES)		// Length of buffer in bytes (= 16,384 bytes)


// -----------------------------------------------------------------------------------------------
// -------------------------------- I2S CONNECTION FOR BT TO MCU ---------------------------------
// -----------------------------------------------------------------------------------------------

static void i2s_bt_to_mcu_to_dac_init ()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	chan_cfg.dma_desc_num = 16;
	chan_cfg.dma_frame_num = 512;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &mcu_tx, &mcu_rx));
    
	i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
	clk_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

	i2s_std_config_t std_cfg = {
	    .clk_cfg = clk_config,
	    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
	    .gpio_cfg = {
			.mclk = I2S_GPIO_UNUSED,
			.bclk = I2S_BCLK_PIN,
			.ws   = I2S_LRCLK_PIN,
			.dout = I2S_DOUT_LINE,
			.din  = I2S_GPIO_UNUSED,
			.invert_flags = {.ws_inv = false}
	    },
	};
	
	// Initialize Full-Duplex Standard mode and enable both channels
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_tx, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_enable(mcu_tx));
    //ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_rx, &std_cfg));
    //ESP_ERROR_CHECK(i2s_channel_enable(mcu_rx));
}// end of i2s_bt_to_mcu_init


static void configure_spiffs()
{
	esp_err_t err;
	
	esp_vfs_spiffs_conf_t spiffs_cfg = {
		.base_path 				= "/storage",
		.partition_label 		= NULL,
		.max_files		 		= 4,
		.format_if_mount_failed = true
	};
	
	err = esp_vfs_spiffs_register(&spiffs_cfg);
	
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Unable to mount SPIFFS, ERROR: %s", esp_err_to_name(err));
		return;
	}
}// end of configure_spiffs function

static void bm83_wakeup()
{
	gpio_set_direction(BT_WAKE, GPIO_MODE_OUTPUT);
	gpio_set_level(BT_WAKE, 1);   		// Pull MFB high
	vTaskDelay(pdMS_TO_TICKS(10));    	// Hold for ~5 ms (safely above 2 ms minimum)
	gpio_set_level(BT_WAKE, 0);   		// Release
	vTaskDelay(pdMS_TO_TICKS(200));      // Brief settle before sending UART
}

static void print_wav(wave_header_t *wav_head)
{
	ESP_LOGI(WAV_FILE, "Bytes per sample: %u", wav_head->bytes_per_sample);
	ESP_LOGI(WAV_FILE, "# of channels: %u", wav_head->n_channels);
	ESP_LOGI(WAV_FILE, "Sample Rate: %u", wav_head->sample_rate);
	ESP_LOGI(WAV_FILE, "Samples per Channel: %u", wav_head->samples_per_channel);
}

void vTask1(void* args)
{
	wave_header_t wav_head;
		wav_hdl = wave_reader_open("/storage/1khz_sine_44_1.wav");
			
		if (wav_hdl == NULL) {
			ESP_LOGE(WAV_FILE, "Unable to open read!");
		}
		
		if (wave_read_header(wav_hdl, &wav_head) == 0) {
			print_wav(&wav_head);
		} else {
			ESP_LOGE(WAV_FILE, "Unable to read WAV file header!");
			return;
		}
		
		uint8_t out;
		uint8_t *buff = (uint8_t *)calloc(1, BUFFER_BYTES);	// Allocate space to store data coming from BT module
		assert(buff);	
		size_t wrote, pos = 0;
		
		while (1)
		{
			//i2s_channel_read(mcu_rx, buff, BUFFER_BYTES, &bytes_read, portMAX_DELAY);		// Read data from BM83
			size_t bytes_read = wave_read_raw_data(wav_hdl, buff, pos, BUFFER_BYTES);		// Read from WAV file
			
			if (bytes_read == 0){
				ESP_LOGW(TAG, "WAV playback finished!");
				break;
			}
			
			pos += bytes_read;
			size_t bytes_to_w = bytes_read;
			uint8_t *p = buff;
			
			while (bytes_to_w > 0) {
				wrote = 0;
				esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY);
				
				if (r != ESP_OK){
					ESP_LOGE(TAG, "I2S threw ERROR: %d", r);
					break;
				}
				
				if (wrote == 0) {
					vTaskDelay(pdMS_TO_TICKS(1));
					continue;
				}
				bytes_to_w -= wrote;
				p += wrote;
			}// end of inner while loop
			//taskYIELD();
		}// end of main while loop 
		i2s_channel_disable(mcu_tx);
		wave_reader_close(wav_hdl);	// close wav file
		free(buff);
		vTaskDelete(NULL);
}

extern "C" void app_main()
{	
	uart_driver_delete(UART_NUM_0);
	gpio_reset_pin(GPIO_NUM_43);
	gpio_reset_pin(GPIO_NUM_44);
	gpio_set_direction(GPIO_NUM_43, GPIO_MODE_INPUT);
	gpio_set_direction(GPIO_NUM_44, GPIO_MODE_INPUT);
    i2s_bt_to_mcu_to_dac_init();
	configure_spiffs();
	
	xTaskCreatePinnedToCore(vTask1, "Playback", 8192, NULL, configMAX_PRIORITIES - 1, NULL, 1);
	
	
}// end of main
