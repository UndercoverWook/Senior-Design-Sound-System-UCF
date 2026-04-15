#ifndef MAIN_MY_FUNCTIONS_BLE_CONTROL_H_
#define MAIN_MY_FUNCTIONS_BLE_CONTROL_H_

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Initialize NimBLE GATT server used by the Flutter control app.
void ble_control_init(void);

// Publish the 8-band histogram expected by the Flutter app.
// fft_complex is the interleaved complex FFT buffer returned by compute_fft().
void ble_publish_fft_bins_from_complex(const float *fft_complex, float sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_MY_FUNCTIONS_BLE_CONTROL_H_ */
