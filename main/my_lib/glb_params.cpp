/*
 * glb_params.cpp
 *
 *  Created on: Mar 30, 2026
 *      Author: matia
 */

#include "glb_params.h"


gptimer_handle_t     sync_timer  = NULL;
spi_device_handle_t  spi_hdl     = NULL;
i2s_chan_handle_t    mcu_tx      = NULL;
i2s_chan_handle_t    mcu_rx      = NULL;
wave_reader_handle_t wav_hdl     = NULL;
TaskHandle_t         task1_hdl   = NULL;
TaskHandle_t         task2_hdl   = NULL;
RingbufHandle_t      audio_ringbuf = NULL;

float cal_values[256][2]    = {};

const char *STORAGE_TAG  = "File System";
const char *SPI_TAG      = "SPI Configuration";
const char *I2S_TAG      = "I2S Configuration";
const char *MIC_TAG      = "EMM6 Calibration";
const char *TEST_TAG     = "Test Signal Playback";
const char *SAMPLING_TAG = "ADC Sampling";
const char *TONE_TAG     = "TONE";
const char *WAV_TAG      = "WAV Test";
const char *EQ_TAG       = "Auto EQ";