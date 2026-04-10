/*
 * usb_device.cpp
 *
 *  Created on: Apr 6 2026
 *  Author: Matias D. Segura
 */

#include <math.h>
#include "my_usb_device.h"

static bool is_muted = false;
static uint32_t volume_factor = 100;

esp_err_t usb_uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    xRingbufferSend(audio_ringbuf, buf, len, 0); // non-blocking
    return ESP_OK;
}

void usb_uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    is_muted = mute;
}

void usb_uac_device_set_volume_cb(uint32_t _volume, void *arg)
{
	int volume_db = _volume / 2 - 50;
	volume_factor = pow(10, volume_db / 20.0f) * 100.0f;
}

void usb_uac_device_init(void)
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
