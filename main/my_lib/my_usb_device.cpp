/*
 * usb_device.cpp
 *
 *  Created on: Apr 6 2026
 *  Author: Matias D. Segura
 */

#include <math.h>
#include "my_usb_device.h"

static bool is_muted = false;
static float volume_scale = 1.0f;

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
	int volume_db = (int)(_volume / 2U) - 50;
	volume_scale = powf(10.0f, (float)volume_db / 20.0f);
}

static inline int16_t clamp_i16_from_float(float x)
{
    if (x > 32767.0f) return 32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)x;
}

void apply_volume_and_mute(int16_t *pcm, size_t sample_count)
{
    if (pcm == NULL || sample_count == 0) return;
    if (is_muted) {
        memset(pcm, 0, sample_count * sizeof(int16_t));
        return;
    }
    if (fabsf(volume_scale - 1.0f) < 1e-6f) return;
    for (size_t i = 0; i < sample_count; ++i) {
        pcm[i] = clamp_i16_from_float((float)pcm[i] * volume_scale);
    }
}

void usb_uac_device_init(void)
{
	if (audio_ringbuf == NULL) audio_ringbuf = xRingbufferCreate(192 * 16, RINGBUF_TYPE_BYTEBUF);
	
    uac_device_config_t config = {
        .output_cb = usb_uac_device_output_cb,
        .input_cb = NULL,
        .set_mute_cb = usb_uac_device_set_mute_cb,
        .set_volume_cb = usb_uac_device_set_volume_cb,
        .cb_ctx = NULL,
    };
    
    ESP_ERROR_CHECK(uac_device_init(&config));
}
