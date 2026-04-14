/*
 * config_functions.cpp
 *
 * Fixed coexistence version:
 * - WAV / app playback stays on I2S0 using mcu_tx / mcu_rx
 * - BM83 streaming uses dedicated audio_tx / audio_rx on I2S1
 * - teammate's BM83 pin map and format are preserved
 * - histogram-safe GPTimer init order is preserved
 */

#include "config_functions.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"

void configure_spi()
{
    esp_err_t err;

    if (spi_hdl != NULL) {
        return;
    }

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

    spi_device_interface_config_t devcfg = {
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
        .mode           = 0,
        .clock_source   = SPI_CLK_SRC_DEFAULT,
        .clock_speed_hz = 2070000,
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

void configure_i2s_for_wav()
{
    esp_err_t err;

    if (mcu_tx != NULL && mcu_rx != NULL) {
        return;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 512;

    err = i2s_new_channel(&chan_cfg, &mcu_tx, &mcu_rx);
    if (err != ESP_OK) {
        ESP_LOGE(I2S_TAG, "Unable to initialize WAV I2S channel, ERROR: %s", esp_err_to_name(err));
        mcu_tx = NULL;
        mcu_rx = NULL;
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
            .invert_flags = {.ws_inv = false},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_tx, &std_cfg));

    ESP_LOGI(I2S_TAG, "WAV I2S configured on I2S0: TX=%d RX=%d BCLK=%d WS=%d",
             (int)I2S_TX_LINE, (int)I2S_RX_LINE, (int)I2S_BIT_CLK, (int)I2S_LRCLK_PIN);
}

void configure_i2s_for_audio()
{
    esp_err_t err;

    if (audio_tx != NULL && audio_rx != NULL) {
        ESP_LOGI(I2S_TAG, "BM83 I2S already configured");
        return;
    }

    /*
     * BM83 streaming must not fight the WAV path for the same I2S controller.
     * Keep the teammate's proven BM83 pin map and format, but place the BM83
     * bridge on I2S1 using dedicated audio handles.
     */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 512;

    err = i2s_new_channel(&chan_cfg, &audio_tx, &audio_rx);
    if (err != ESP_OK) {
        ESP_LOGE(I2S_TAG, "Unable to initialize BM83 I2S channels, ERROR: %s", esp_err_to_name(err));
        audio_tx = NULL;
        audio_rx = NULL;
        return;
    }

    i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(48000);
    clk_config.clk_src = I2S_CLK_SRC_DEFAULT;
    clk_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = clk_config,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BIT_CLK,
            .ws   = I2S_LRCLK_PIN,
            .dout = I2S_TX_LINE,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {.ws_inv = false},
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
            .invert_flags = {.ws_inv = false},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(audio_tx, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(audio_rx, &rx_std_cfg));



    ESP_LOGI(I2S_TAG,
             "BM83 I2S configured on I2S1: TX=%d RX=%d BCLK=%d WS=%d",
             (int)I2S_TX_LINE,
             (int)I2S_RX_LINE,
             (int)I2S_BIT_CLK,
             (int)I2S_LRCLK_PIN);
}

void configure_spiffs()
{
    esp_err_t err;

    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path              = "/storage",
        .partition_label        = NULL,
        .max_files              = 4,
        .format_if_mount_failed = true,
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
    if (psram_err != ESP_OK) {
        ESP_LOGE(STORAGE_TAG, "Unable to initialize External PSRAM, ERROR: %s", esp_err_to_name(psram_err));
    }
}

void initialize_pacer_timer(gptimer_handle_t *t)
{
    if (t == NULL) {
        return;
    }

    if (*t != NULL) {
        return;
    }

    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = PACER_TIMER_HZ,
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, t));
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
