/*
 * config_functions.cpp
 *
 *  Created on: Mar 20, 2026
 *      Author: Matias Segura
 *
 *	This .cpp file contains all configurations functions for external components
 *	such as ADC, DAC, PSRAM, BM83, PSRAM, as well as protocol initialization, including
 *	SPI, I2S, UART
 */

#include "config_functions.h"

void configure_spi() 
{
    esp_err_t err;

    // SPI BUS configuration
    spi_bus_config_t buscfg = {
        .mosi_io_num     = -1,
        .miso_io_num     = ADC_MISO_PIN,
        .sclk_io_num     = ADC_SCLK_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE,
    };

    err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(STORAGE_TAG, "Failed to initialize SPI bus, ERROR: %s", esp_err_to_name(err));
        return;
    }

    // SPI device configuration
    spi_device_interface_config_t devcfg = {
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
        .mode           = 0,
        .clock_source   = SPI_CLK_SRC_DEFAULT,
        .clock_speed_hz = 2070000, // 2.1 MHz (Max. is 2.4 MHz)
        .spics_io_num   = ADC_CS_PIN,
        .queue_size     = 1,
    };

    err = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(STORAGE_TAG, "Failed to add SPI device, ERROR: %s", esp_err_to_name(err));
        return;
    }

}

void configure_i2s_for_wav()
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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_tx, &std_cfg));
}

void configure_i2s_for_audio()
{
	esp_err_t err;
	i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

	err = i2s_new_channel(&chan_cfg, &mcu_tx, &mcu_rx);
	
	if (err != ESP_OK) {
		ESP_LOGE(I2S_TAG, "Unable to initialize I2S channel, ERROR: %s", esp_err_to_name(err));
		return;
	}
	
	i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(48000);
	clk_config.clk_src = I2S_CLK_SRC_PLL_160M;
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
		},
	};

	// Initialize standard sender channel only
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_rx, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_tx, &std_cfg));

	gpio_set_direction(GPIO_NUM_2, GPIO_MODE_INPUT);
	esp_rom_gpio_connect_out_signal(GPIO_NUM_2, 0x100, false, false);
	//gpio_set_drive_capability(GPIO_NUM_21, GPIO_DRIVE_CAP_1); // BCLK
	//gpio_set_drive_capability(GPIO_NUM_11, GPIO_DRIVE_CAP_1); // WS
}

void configure_spiffs()
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
}

void configure_psram()
{
	esp_err_t psram_err = esp_psram_init();
	if (psram_err != ESP_OK){
		ESP_LOGE(STORAGE_TAG, "Unable to initialize External PSRAM, ERROR: %s", esp_err_to_name(psram_err));
	}
}

void initialize_pacer_timer(gptimer_handle_t *t)
{
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = PACER_TIMER_HZ,
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, t));
    ESP_ERROR_CHECK(gptimer_enable(*t));
}

void reconfigure_wdt()
{
    esp_task_wdt_config_t twdt_cfg = {
	    .timeout_ms = 15000,   			// give margin during 5 s capture
	    .idle_core_mask = (1 << CORE0), // watch only CPU0 idle task
	    .trigger_panic = true,
	};

	esp_err_t twdt_err = esp_task_wdt_reconfigure(&twdt_cfg);
	if (twdt_err != ESP_OK) {
	    ESP_LOGW("TWDT", "TWDT reconfigure failed: %s", esp_err_to_name(twdt_err));
	}
}