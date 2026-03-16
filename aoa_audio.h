#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

bool aoa_is_audio_mode_device(uint16_t vid, uint16_t pid);
bool aoa_is_android_google_vid(uint16_t vid);
esp_err_t aoa_try_enable_audio_mode(usb_host_client_handle_t client_hdl,
                                    usb_device_handle_t dev_hdl);

#ifdef __cplusplus
}
#endif
