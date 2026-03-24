#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

extern "C" {
	#include "freertos/FreeRTOS.h"
	#include "freertos/task.h"
	#include "freertos/ringbuf.h"
	#include "usb_device_uac.h"
	#include "driver/i2s_common.h"
	#include "driver/i2s_types.h"
	#include "driver/i2s_std.h"
	#include "esp_err.h"
	#include "esp_log.h"
}

static const char *TAG = "USB";

static i2s_chan_handle_t Tx;
static bool is_muted = false;
static uint32_t volume_factor = 100;
static const gpio_num_t I2S_LRCLK_PIN = GPIO_NUM_4;		// LRCLK (WS) line connected to the BM83 and DAC
static const gpio_num_t I2S_BCLK_PIN  = GPIO_NUM_5;		// Bit Clock line connected to the BM83 and DAC
static const gpio_num_t I2S_DOUT_LINE = GPIO_NUM_6;		// Data out from MCU to DAC through I2S (DIN for DAC)

#define SAMPLE_RATE		48000
static RingbufHandle_t audio_ringbuf = NULL;

static void i2s_init ()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	chan_cfg.dma_desc_num = 16;
	chan_cfg.dma_frame_num = 512;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &Tx, NULL));
    
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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(Tx, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_enable(Tx));
}

static esp_err_t usb_uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    xRingbufferSend(audio_ringbuf, buf, len, 0); // non-blocking
    return ESP_OK;
}


static void usb_uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    is_muted = mute;
}

static void usb_uac_device_set_volume_cb(uint32_t _volume, void *arg)
{
	int volume_db = _volume / 2 - 50;
	volume_factor = pow(10, volume_db / 20.0f) * 100.0f;
}

static void usb_uac_device_init(void)
{
	audio_ringbuf = xRingbufferCreate(192 * 16, RINGBUF_TYPE_BYTEBUF);
	
    uac_device_config_t config = {
        .output_cb = usb_uac_device_output_cb,
        .input_cb = NULL,
        .set_mute_cb = usb_uac_device_set_mute_cb,
        .set_volume_cb = usb_uac_device_set_volume_cb,
        .cb_ctx = NULL,
    };
    
    ESP_ERROR_CHECK(uac_device_init(&config));
}

static void i2s_write_task(void *arg)
{
    while (1) {
        size_t bytes_received = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
            audio_ringbuf, &bytes_received, portMAX_DELAY, 192);
        if (data) {
            size_t bytes_written = 0;
            i2s_channel_write(Tx, data, bytes_received, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(audio_ringbuf, data);
        }
    }
}


extern "C" void app_main(void)
{
	i2s_init();
	usb_uac_device_init();
	
	xTaskCreate(i2s_write_task, "i2s_write", 4096, NULL, 5, NULL);
	
	while (1){
		vTaskDelay(1000);	
	}
}
