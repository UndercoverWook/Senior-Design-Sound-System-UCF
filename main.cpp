#include "esp_dsp.h"
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

extern "C" {
	#include "freertos/FreeRTOS.h"
	#include "freertos/semphr.h"
	#include "driver/spi_common.h"
	#include "driver/spi_master.h"
	#include "driver/gptimer.h"
	#include "esp_log.h"
	#include "hal/spi_types.h"
	#include "soc/gpio_num.h"
	#include "driver/i2s_std.h"
	#include "esp_spiffs.h"
	#include "esp_err.h"
	#include "freertos/idf_additions.h"
	#include "soc/clk_tree_defs.h"
	#include "esp_heap_caps.h"
	#include "esp_psram.h"
	#include "esp_task_wdt.h"
	#include "esp_log_timestamp.h"
	#include "driver/gpio.h"
	#include "driver/i2s_types.h"
	#include "wave.h"
	#include "wave_common.h"
	#include "wave_reader.h"
	#include "esp_rom_sys.h"
	#include "hal/i2s_types.h"
}

// General parameters
#define SAMPLE_RATE		 44100								 // Sample rate, 44.1kHz enough to avoid undersampling but 48kHz is cleaner
#define TEST_DURATION	 5									 // Duration of Test Signal
#define BUFFER_FRAMES  	 1024            					 // frames (stereo frames) captured and transmitted 
#define BYTES_PER_SAMPLE 2               					 // 16-bit => 2 bytes per channel sample
#define CHANNELS         2									 // Stereo = 2 || Mono = 1
#define FRAME_SIZE_BYTES (BYTES_PER_SAMPLE * CHANNELS)  	 // 4 bytes per frame (16-bit stereo)
#define BUFFER_BYTES     (BUFFER_FRAMES * FRAME_SIZE_BYTES)
#define FILE_PATH		 "test.wav"
#define PACER_TIMER_HZ   10000000ULL   						 // 10 MHz => 0.1 us ticks
#define FFT_SIZE 		 2048
#define NUM_BINS		 (FFT_SIZE / 2)
#define FREQ_START		 20.0f
#define FREQ_END		 20000.0f
#define AMPLITUDE		 0x3FFFFF
#define CORE0			 0
#define CORE1			 1

// GPIOs Declarations
static const gpio_num_t MCU_WAKE	 	= GPIO_NUM_1;		// Wake up the MCU (for the BM83)
static const gpio_num_t I2S_RX_LINE  	= GPIO_NUM_2;		// Data out from BM83 to MCU through I2S
static const gpio_num_t BUS_SENSE	 	= GPIO_NUM_6;		// ?
static const gpio_num_t HIGH_FAULT	 	= GPIO_NUM_7;		// ?
static const gpio_num_t BT_UART_TXD	 	= GPIO_NUM_8;		// UART transmitting line from BM83 (RX for the MCU)
static const gpio_num_t BT_UART_RXD  	= GPIO_NUM_9;		// UART receving line from BM83 (TX for the MCU)
static const gpio_num_t ADC_CS_PIN   	= GPIO_NUM_10;		// Chip Select line for ADS8320
static const gpio_num_t I2S_LRCLK_PIN 	= GPIO_NUM_11;		// Left Right Clock (aka WS = Word Select) line for I2S
static const gpio_num_t ADC_SCLK_PIN 	= GPIO_NUM_12;		// Master Clock line for ADS8320
static const gpio_num_t ADC_MISO_PIN	= GPIO_NUM_13;		// Master In Slave Out line for ADS8320
static const gpio_num_t I2S_TX_LINE 	= GPIO_NUM_14;		// Transmitting line for I2S protocol
static const gpio_num_t HIGH_MUTE	 	= GPIO_NUM_15;		// ?
static const gpio_num_t SUB_CLIP	 	= GPIO_NUM_16;		// ?
static const gpio_num_t SUB_FAULT	 	= GPIO_NUM_17;		// ?
static const gpio_num_t SUB_RST		 	= GPIO_NUM_18;		// ?
static const gpio_num_t USB_D_MINUS	 	= GPIO_NUM_19;		// Positive data line for USB-A connector
static const gpio_num_t USB_D_PLUS	 	= GPIO_NUM_20;		// Negative data line for USB-A connector
static const gpio_num_t I2S_BIT_CLK    	= GPIO_NUM_21;		// Bit Clock line for I2S protocol
static const gpio_num_t BT_WAKE 	 	= GPIO_NUM_48;		// Used to signal the BM83 to go into 'Pair' mode

// Handles
static gptimer_handle_t sync_timer  = NULL;
static spi_device_handle_t spi_hdl	= NULL;
static i2s_chan_handle_t mcu_tx 	= NULL;
static i2s_chan_handle_t mcu_rx		= NULL;
TaskHandle_t task1_hdl 				= NULL;
TaskHandle_t task2_hdl 				= NULL;
static wave_reader_handle_t wav_hdl = NULL;

static float wind[FFT_SIZE];        // Window coefficients
static float y_cf[FFT_SIZE * 2];    // Interleaved complex input: [r0, i0, r1, i1, etc.]
static float mag[NUM_BINS];         // Magnitude output
static float cal_values [256][2];		// To store calibration values as a pair of values in a 2D array fashion
static const uint8_t  DATA_LENGTH = 24;	// A full conversion lasts 24 cycles (mask result to get 16 bits)
static const uint32_t STACK_DEPTH = 8192;

// TAGS:
static const char *STORAGE_TAG 	= "File System";
static const char *SPI_TAG 	   	= "SPI Configuration";
static const char *I2S_TAG 	   	= "I2S Configuration";
static const char *MIC_TAG	   	= "EMM6 Calibration";
static const char *TEST_TAG	   	= "Test Signal Playback";
static const char *SAMPLING_TAG = "ADC Sampling";
static const char *WAV_FILE 	= "WAV Test";

// Timer callback for ADC sampling
static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    xSemaphoreGiveFromISR(sem, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////// FUNCTIONS ///////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////


// Configure SPI connection for ADC unit
static void configure_spi()
{
	esp_err_t err;
	
	// SPI Bus configuration & initialization
	spi_bus_config_t spi_bus_cfg = {
		.mosi_io_num 	 = -1,			// Reading only
		.miso_io_num 	 = ADC_MISO_PIN,
		.sclk_io_num 	 = ADC_SCLK_PIN,
		.quadwp_io_num 	 = -1,
		.quadhd_io_num 	 = -1,
		.max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE
	};
	
	err = spi_bus_initialize(SPI2_HOST, &spi_bus_cfg, SPI_DMA_DISABLED);
	
	if (err != ESP_OK){
		ESP_LOGE(SPI_TAG, "Failed to initialize bus, ERROR: %s", esp_err_to_name(err));
		return;
	}
	
	spi_device_interface_config_t ADS8320_cfg = {
		.command_bits 	  = 0,
		.address_bits 	  = 0,
		.dummy_bits	  	  = 0,
		.mode		  	  = 0,
		.clock_source 	  = SPI_CLK_SRC_DEFAULT,
		.clock_speed_hz	  = 2000000,
		.input_delay_ns	  = 0,
		.spics_io_num 	  = ADC_CS_PIN,
		.queue_size		  = 1
	};
	
	err = spi_bus_add_device(SPI2_HOST, &ADS8320_cfg, &spi_hdl);
	
	if (err != ESP_OK){
		ESP_LOGE(SPI_TAG, "Failed to add device to the SPI bus, ERROR: %s", esp_err_to_name(err));
		return;
	}
}// end of configure_spi function

// Configure I2S protocol por pipeline: BT MODULE --> ESP32-S3 --> DAC
static void configure_i2s()
{
	esp_err_t err;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	chan_cfg.dma_desc_num = 16;
	chan_cfg.dma_frame_num = 512;

    err = i2s_new_channel(&chan_cfg, &mcu_tx, &mcu_rx);
	
	if (err != ESP_OK) {
		ESP_LOGE(I2S_TAG, "Unable to initialize I2S channel, ERROR: %s", esp_err_to_name(err));
		return;
	}
    
    i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    clk_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_config,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BIT_CLK,
            .ws   = I2S_LRCLK_PIN,
            .dout = I2S_TX_LINE,
            .din  = I2S_RX_LINE,
			.invert_flags = {.ws_inv = false}
        },
    };

	// Initialize standard sender channel only
    //ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_tx, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_enable(mcu_tx));
}// end of configure_i2s function

// Configure SPIFFS File System
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
		ESP_LOGE(STORAGE_TAG, "Unable to mount SPIFFS, ERROR: %s", esp_err_to_name(err));
		return;
	}
}// end of configure_spiffs function

// Configure external PSRAM (returns a pointer to allocated buffer for sampled data)
static void configure_psram()
{
	esp_err_t psram_err = esp_psram_init();
	if (psram_err != ESP_OK){
		ESP_LOGE(STORAGE_TAG, "Unable to initialize External PSRAM, ERROR: %s", esp_err_to_name(psram_err));
	}
}// end of configure_psram function

// Pacer timer initialization for synchronization
static void initialize_pacer_timer(gptimer_handle_t *t)
{
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = PACER_TIMER_HZ,
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, t));
    ESP_ERROR_CHECK(gptimer_enable(*t));
}// end of pacer timer initialization

static void bm83_wakeup ()
{
	gpio_set_direction(BT_WAKE, GPIO_MODE_OUTPUT);
	gpio_set_level(BT_WAKE, 1);   // Pull MFB high
	vTaskDelay(pdMS_TO_TICKS(10));  // Hold for ~5 ms (safely above 2 ms minimum)
	gpio_set_level(BT_WAKE, 0);   // Release
	vTaskDelay(pdMS_TO_TICKS(2));   // Brief settle before sending UART
}

void compute_fft_and_print(uint16_t *samples, int num_samples)
{
    // Generate Hann window
    dsps_wind_hann_f32(wind, FFT_SIZE);

    // Normalize samples to [-1.0, 1.0] and apply window
    // ADS8320 is 16-bit unsigned, midpoint is 32768
    for (int i = 0; i < FFT_SIZE; i++) {
        float normalized = (samples[i] - 32768.0f) / 32768.0f;
        y_cf[i * 2 + 0] = normalized * wind[i];  // Real part
        y_cf[i * 2 + 1] = 0.0f;                  // Imaginary part (zero for real input)
    }

    // Run FFT
    esp_err_t ret = dsps_fft2r_fc32(y_cf, FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE("FFT", "FFT failed: %s", esp_err_to_name(ret));
        return;
    }

    // Bit-reverse the output
    dsps_bit_rev_fc32(y_cf, FFT_SIZE);

    // Compute magnitude for each bin
    // Each bin k corresponds to frequency: k * (SAMPLE_RATE / FFT_SIZE)
    for (int i = 0; i < NUM_BINS; i++) {
        float re = y_cf[i * 2 + 0];
        float im = y_cf[i * 2 + 1];
		mag[i] = (sqrtf(re * re + im * im) / (FFT_SIZE / 2)) * 2.0f;
    }

    // Print results 
    ESP_LOGI("FFT", "Frequency (Hz) | Magnitude (dB)");
    for (int i = 1; i < NUM_BINS; i++) {  
        float freq_hz  = (float)i * SAMPLE_RATE / FFT_SIZE;
		if (freq_hz > 20100.0f) break;									// Break when above 20k
		float mag_db = 10.0f * log10f(mag[i] + 1e-9f);
        ESP_LOGI("FFT", "%8.1f Hz | %.2f dB", freq_hz, mag_db);
    }
}

// Read the EMM Calibration file and save it as an array
static void file_to_arr () 
{
	FILE* fptr = fopen("/storage/EMM6_calibration.txt", "r");
	if (fptr == NULL) {
		ESP_LOGE(MIC_TAG, "Failed to open file.");
		return;
	}
		
	float frequency, magnitude;
		
	char line [15];						// To store the first line of the file
	fgets(line, 15, fptr);				// Consume the first line (Irrelevant)
	uint16_t i = 0;
		
	while(i < 256 && fscanf(fptr, "%f\t%f", &frequency, &magnitude) == 2)
	{
		cal_values[i][0] = frequency;
		cal_values[i][1] = magnitude;
		i++;
			
		if (i % 10 == 0)
			vTaskDelay(1);
	}
	fclose(fptr);
	return;
}// end of file_to_arr function

static void print_wav(wave_header_t *wav_head)
{
	ESP_LOGI(WAV_FILE, "Bytes per sample: %u", wav_head->bytes_per_sample);
	ESP_LOGI(WAV_FILE, "# of channels: %u", wav_head->n_channels);
	ESP_LOGI(WAV_FILE, "Sample Rate: %u", wav_head->sample_rate);
	ESP_LOGI(WAV_FILE, "Samples per Channel: %u", wav_head->samples_per_channel);
}

/*static void sample_to_file(uint16_t *samples)
{
	FILE* fptr = fopen("Sample_1.txt", "w");
	if (fptr == NULL) {
		ESP_LOGE(STORAGE_TAG, "Failed to create file.");
		return;
	}
	
	for (int i = 0 ; i < TEST_DURATION * SAMPLE_RATE ; i++) {
		fprintf(fptr,"%d/n", samples[i]);
	}
	ESP_LOGI(STORAGE_TAG, "Array successfully written to file!");
}*/

/////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////// TASKS /////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////


// ADC (External) Sampling Task to capture the test signal
void vSampleTask(void *args)
{
	spi_transaction_t trans = {
		.flags		= SPI_TRANS_USE_RXDATA,
		.length		= DATA_LENGTH,
		.rxlength	= DATA_LENGTH,
	};
	
	uint16_t* samples = (uint16_t *) args;
	int max_samples = SAMPLE_RATE * TEST_DURATION;

	ESP_ERROR_CHECK(gptimer_start(sync_timer));
	
	uint32_t t_start = esp_log_timestamp(); 	// Retrieve start time
	for (int i = 0 ; i < max_samples ; i++) {
	   	spi_device_polling_transmit(spi_hdl, &trans);
		uint32_t raw = ((uint32_t)trans.rx_data[0] << 16) | ((uint32_t)trans.rx_data[1] << 8) | (uint32_t)trans.rx_data[2];
		samples[i] = (raw >> 2) & 0xFFFF;
	}
	uint32_t t_end  = esp_log_timestamp();		// Retrieve end time
	ESP_LOGI(SAMPLING_TAG, "Sampling Finished!");
	float actual_fs = (float)max_samples / ((t_end - t_start) / 1000.0f);	// Calculate actual sampling rate
	ESP_LOGI("SAMPLE", "Actual sample rate: %.1f Hz", actual_fs);
	
	//sample_to_file(samples);	// Write array to txt file
	
	ESP_LOGI(SAMPLING_TAG, "Running FFT now...");
	compute_fft_and_print(samples, max_samples);
	
	vTaskDelete(NULL);		// Self-destroy
}// end of vSampleTask

// Test signal playback through the DAC
void vTestSignalTask(void *args)
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
	
	uint8_t *buff = (uint8_t *)calloc(1, BUFFER_BYTES);	// Allocate space to store data coming from BT module
	assert(buff);	
	size_t wrote, pos = 0;
			
	while (1)
	{
		size_t bytes_read = wave_read_raw_data(wav_hdl, buff, pos, BUFFER_BYTES);		// Read from WAV file
				
		if (bytes_read == 0){
			ESP_LOGW(TEST_TAG, "WAV playback finished!");
			break;
		}
				
		pos += bytes_read;
		size_t bytes_to_w = bytes_read;
		uint8_t *p = buff;
				
		while (bytes_to_w > 0) {
			wrote = 0;
			esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_to_w, &wrote, portMAX_DELAY);
			
			if (r != ESP_OK){
				ESP_LOGE(TEST_TAG, "I2S threw ERROR: %d", r);
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


/////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////// MAIN //////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////


extern "C" void app_main(void)
{
	// Configure File System to include personal partition (where useful files are located)
	// and initialize PSRAM module to store big buffers
	configure_spiffs();
	configure_psram();
	uint16_t *samples = (uint16_t *)heap_caps_malloc(SAMPLE_RATE * TEST_DURATION * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	
	// Configure SPI and I2S protocols for sampling and test signal playback
	// Sampling is done using the external ADC unit (ADS8320) which communicates via SPI
	// The test signal is stored in flash and retrieved at the time of sampling, played 
	// 		through the external DAC (PCM5122) which communicates via I2S.
	configure_spi();
	configure_i2s();
	bm83_wakeup();
	
	// Initialize timer
	initialize_pacer_timer(&sync_timer);
	
	esp_task_wdt_config_t twdt_cfg = {
	    .timeout_ms = 15000,   			// give margin during 5 s capture
	    .idle_core_mask = (1 << CORE0), // watch only CPU0 idle task
	    .trigger_panic = true,
	};

	esp_err_t twdt_err = esp_task_wdt_reconfigure(&twdt_cfg);
	if (twdt_err != ESP_OK) {
	    ESP_LOGW("TWDT", "TWDT reconfigure failed: %s", esp_err_to_name(twdt_err));
	}
	
	esp_err_t ret = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
	if (ret != ESP_OK) {
		ESP_LOGE("MAIN", "FFT init failed: %s", esp_err_to_name(ret));
	    return;
	}
	
	// Create Sampling and Playback tasks and assign them a different Core Affinity (with the same priority)
	xTaskCreatePinnedToCore(vSampleTask, "ADC Sampling", STACK_DEPTH, (void *) samples, configMAX_PRIORITIES - 1, &task1_hdl, CORE1);	
	//xTaskCreatePinnedToCore(vTestSignalTask, "Test Signal", STACK_DEPTH, NULL, configMAX_PRIORITIES - 1,NULL, CORE0);
}