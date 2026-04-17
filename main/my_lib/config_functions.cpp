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
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"

static void delete_existing_i2s_channels()
{
    if (mcu_rx != NULL) {
        esp_err_t err = i2s_channel_disable(mcu_rx);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(I2S_TAG, "RX channel disable returned: %s", esp_err_to_name(err));
        }
        i2s_del_channel(mcu_rx);
        mcu_rx = NULL;
    }

    if (mcu_tx != NULL) {
        esp_err_t err = i2s_channel_disable(mcu_tx);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(I2S_TAG, "TX channel disable returned: %s", esp_err_to_name(err));
        }
        i2s_del_channel(mcu_tx);
        mcu_tx = NULL;
    }
}

void configure_spi() 
{
    esp_err_t err;

    if (spi_hdl != NULL) {
        return;
    }

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
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
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
        spi_hdl = NULL;
        return;
    }
}

void configure_i2s_for_wav(uint32_t sample_rate_hz, bool stereo_output)
{
    esp_err_t err;
    delete_existing_i2s_channels();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 512;

    err = i2s_new_channel(&chan_cfg, &mcu_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(I2S_TAG, "Unable to initialize I2S channel, ERROR: %s", esp_err_to_name(err));
        return;
    }

    if (sample_rate_hz == 0) {
        sample_rate_hz = SAMPLE_RATE;
    }

    i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    clk_config.clk_src = I2S_CLK_SRC_DEFAULT;
    clk_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_slot_mode_t slot_mode = stereo_output ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, slot_mode);
    if (!stereo_output) {
        slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_config,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BIT_CLK,
            .ws   = I2S_LRCLK_PIN,
            .dout = I2S_TX_LINE,
            .din  = I2S_GPIO_UNUSED,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(mcu_tx));

    ESP_LOGI(I2S_TAG,
             "WAV I2S configured: fs=%lu Hz, slot_mode=%s, bit_width=16",
             (unsigned long)sample_rate_hz,
             stereo_output ? "stereo" : "mono");
}

void configure_i2s_for_audio(bool bluetooth)
{
    esp_err_t err;
    delete_existing_i2s_channels();
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 512;

    if (bluetooth){
        err = i2s_new_channel(&chan_cfg, &mcu_tx, &mcu_rx);
    } else {
        err = i2s_new_channel(&chan_cfg, &mcu_tx, NULL);
    }

    if (err != ESP_OK) {
        ESP_LOGE(I2S_TAG, "Unable to initialize I2S channel, ERROR: %s", esp_err_to_name(err));
        return;
    }

    i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(48000);
    clk_config.clk_src = I2S_CLK_SRC_DEFAULT;
    clk_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    // 2. CONFIG FOR TX (Output to DAC)
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = clk_config,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BIT_CLK,
            .ws   = I2S_LRCLK_PIN,
            .dout = I2S_TX_LINE,
            .din  = I2S_GPIO_UNUSED,
        },
    };

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = clk_config,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BIT_CLK,
            .ws   = I2S_LRCLK_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_RX_LINE,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_tx, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(mcu_tx));
    if (bluetooth) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_rx, &rx_std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(mcu_rx));
    }
}

void configure_spiffs()
{
    esp_err_t err;

    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path               = "/storage",
        .partition_label         = NULL,
        .max_files               = 4,
        .format_if_mount_failed  = true
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
        .timeout_ms = 15000,
        .idle_core_mask = (1 << CORE0),
        .trigger_panic = true,
    };

    esp_err_t twdt_err = esp_task_wdt_reconfigure(&twdt_cfg);
    if (twdt_err != ESP_OK) {
        ESP_LOGW("TWDT", "TWDT reconfigure failed: %s", esp_err_to_name(twdt_err));
    }
}
