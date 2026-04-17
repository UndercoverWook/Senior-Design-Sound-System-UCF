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

volatile bool flush_required = false;

void apply_volume_and_mute(int16_t *samples, size_t num_samples)
{
    if (is_muted) {
        memset(samples, 0, num_samples * sizeof(int16_t));
        return;
    }
    if (volume_factor == 100) return; // unity gain, skip processing

    for (size_t i = 0; i < num_samples; i++) {
        int32_t scaled = ((int32_t)samples[i] * (int32_t)volume_factor) / 100;
        // Clamp to int16 range
        if (scaled >  32767) scaled =  32767;
        if (scaled < -32768) scaled = -32768;
        samples[i] = (int16_t)scaled;
    }
}

esp_err_t usb_uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    static TickType_t last_cb_tick = 0;
    TickType_t now = xTaskGetTickCount();

    if (last_cb_tick != 0 && (now - last_cb_tick) > pdMS_TO_TICKS(50)) {
        ESP_LOGW("UAC", "Gap detected: %lums - flushing",
                 (now - last_cb_tick) * portTICK_PERIOD_MS);
        flush_required = true;  // signal the playback task
    }
    last_cb_tick = now;

    BaseType_t result = xRingbufferSend(audio_ringbuf, buf, len, pdMS_TO_TICKS(10));
    if (result != pdTRUE) {
        ESP_LOGW("UAC", "Ring buffer full, dropped %d bytes", len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void usb_uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    is_muted = mute;
}

void usb_uac_device_set_volume_cb(uint32_t _volume, void *arg)
{
	float volume_db = ((float)_volume / 100.0f) * 60.0f - 60.0f;
    volume_factor = (uint32_t)(powf(10.0f, volume_db / 20.0f) * 100.0f);
    ESP_LOGI("UAC", "Volume: raw=%lu, dB=%.2f, factor=%lu", _volume, volume_db, volume_factor);
}

void usb_uac_device_init(void)
{
	audio_ringbuf = xRingbufferCreate(192 * 100, RINGBUF_TYPE_BYTEBUF);
	
    uac_device_config_t config = {
        .output_cb = usb_uac_device_output_cb,
        .input_cb = NULL,
        .set_mute_cb = usb_uac_device_set_mute_cb,
        .set_volume_cb = usb_uac_device_set_volume_cb,
        .cb_ctx = NULL,
    };
    
    ESP_ERROR_CHECK(uac_device_init(&config));
}