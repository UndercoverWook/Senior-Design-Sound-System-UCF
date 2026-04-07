#ifndef MAIN_MY_FUNCTIONS_CONFIG_FUNCTIONS_H_
#define MAIN_MY_FUNCTIONS_CONFIG_FUNCTIONS_H_

#pragma once
#include "glb_params.h"

#ifdef __cplusplus
extern "C" {
#endif


// Configure SPI connection for ADC unit
void configure_spi();

// Configure I2S protocol for external WAV playback
void configure_i2s_for_wav();

// Configure I2S protocol for audio streaming (BM83 or USB Audio)
void configure_i2s_for_audio();

// Configure SPIFFS File System
void configure_spiffs();

// Configure external PSRAM (returns a pointer to allocated buffer for sampled data)
void configure_psram();

void initialize_pacer_timer(gptimer_handle_t *t);

// Reconfigure Task Watchdog Timer to avoid false positives during long computations
void reconfigure_wdt();

#ifdef __cplusplus
}
#endif

#endif /* MAIN_MY_FUNCTIONS_CONFIG_FUNCTIONS_H_ */