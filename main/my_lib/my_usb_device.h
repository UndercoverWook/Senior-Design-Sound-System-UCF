#ifndef MAIN_MY_FUNCTIONS_USB_DEVICE_H_
#define MAIN_MY_FUNCTIONS_USB_DEVICE_H_

#pragma once
#include "glb_params.h"

#ifdef __cplusplus
extern "C" {
#endif

// USB UAC device callback 
esp_err_t usb_uac_device_output_cb(uint8_t *buf, size_t len, void *arg);

// Sets the mute state of the USB device
void usb_uac_device_set_mute_cb(uint32_t mute, void *arg);

// Sets the volume of the USB device
void usb_uac_device_set_volume_cb(uint32_t _volume, void *arg);

// Initializes the USB UAC device with the specified configuration
void usb_uac_device_init(void);


#ifdef __cplusplus
}
#endif

#endif /* MAIN_MY_FUNCTIONS_CONFIG_FUNCTIONS_H_ */