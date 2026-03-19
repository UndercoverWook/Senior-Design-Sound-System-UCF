#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include <inttypes.h>

#include "usb_device_uac.h"

static const char *TAG = "usb_phone_audio";

/*
 * ---------------------------------------------------------
 * BOARD-SPECIFIC SETTINGS
 * ---------------------------------------------------------
 * Based on your current design discussion, your PCM5122 DAC is
 * fed from the ESP32-S3 over I2S.
 *
 * CHANGE THESE IF YOUR REAL PCB USES DIFFERENT I2S NETS.
 */
#define I2S_BCLK_GPIO   GPIO_NUM_12
#define I2S_WS_GPIO     GPIO_NUM_11
#define I2S_DOUT_GPIO   GPIO_NUM_13
#define I2S_MCLK_GPIO   I2S_GPIO_UNUSED   // Set a real GPIO if your DAC needs MCLK

/*
 * Optional mute / amp enable pin if you have one.
 * Leave as GPIO_NUM_NC if unused.
 */
#define AMP_EN_GPIO     GPIO_NUM_NC

/*
 * USB audio settings.
 * Keep these aligned with menuconfig for USB Device UAC.
 *
 * Android USB audio commonly works well with:
 *   48 kHz, 16-bit, stereo
 *
 * If your host negotiates a different rate through the UAC component
 * configuration, make menuconfig and I2S agree.
 */
#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      2

static i2s_chan_handle_t s_tx_chan = NULL;
static volatile bool s_muted = false;
static volatile int s_volume_db = 0;

/*
 * ---------------------------------------------------------
 * I2S / DAC setup
 * ---------------------------------------------------------
 */
static void init_i2s_output(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "I2S TX ready: BCLK=%d WS=%d DOUT=%d MCLK=%d",
             I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO, I2S_MCLK_GPIO);
}

/*
 * ---------------------------------------------------------
 * USB UAC callbacks
 * ---------------------------------------------------------
 * output_cb:
 *   Host -> ESP speaker stream
 *   We forward that PCM audio to the PCM5122 over I2S.
 */
static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *cb_ctx)
{
    (void)cb_ctx;

    if (s_tx_chan == NULL || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_muted) {
        memset(buf, 0, len);
    }

    size_t total_written = 0;
    while (total_written < len) {
        size_t just_written = 0;
        esp_err_t err = i2s_channel_write(
            s_tx_chan,
            buf + total_written,
            len - total_written,
            &just_written,
            portMAX_DELAY
        );

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
            return err;
        }

        total_written += just_written;
    }

    return ESP_OK;
}

/*
 * We are not using the microphone/input direction right now.
 * Returning zero bytes is fine for speaker-only operation.
 */
static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *cb_ctx)
{
    (void)buf;
    (void)len;
    (void)cb_ctx;

    if (bytes_read) {
        *bytes_read = 0;
    }
    return ESP_OK;
}

static void uac_set_mute_cb(uint32_t mute, void *cb_ctx)
{
    (void)cb_ctx;
    s_muted = (mute != 0);
    ESP_LOGI(TAG, "USB audio mute set to %s", s_muted ? "ON" : "OFF");
}

static void uac_set_volume_cb(uint32_t volume, void *cb_ctx)
{
    (void)cb_ctx;

    /*
     * The component passes host volume data here.
     * The exact volume encoding depends on host/UAC handling.
     * For now, store and log it. Actual DSP gain scaling can be added later.
     */
    s_volume_db = (int)volume;
    ESP_LOGI(TAG, "USB audio volume changed: raw=%" PRIu32, volume);
}

/*
 * ---------------------------------------------------------
 * Main
 * ---------------------------------------------------------
 */
void app_main(void)
{
    if (AMP_EN_GPIO != GPIO_NUM_NC) {
        gpio_reset_pin(AMP_EN_GPIO);
        gpio_set_direction(AMP_EN_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(AMP_EN_GPIO, 1);
        ESP_LOGI(TAG, "Amplifier enabled on GPIO %d", AMP_EN_GPIO);
    }

    init_i2s_output();

    uac_device_config_t uac_cfg = {
        .skip_tinyusb_init = false,
        .output_cb = uac_output_cb,
        .input_cb = NULL,              // speaker-only for now
        .set_mute_cb = uac_set_mute_cb,
        .set_volume_cb = uac_set_volume_cb,
        .cb_ctx = NULL,
    };

    ESP_ERROR_CHECK(uac_device_init(&uac_cfg));

    ESP_LOGI(TAG, "USB UAC device initialized");
    ESP_LOGI(TAG, "Waiting for Android phone to enumerate this board as a USB audio device...");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}