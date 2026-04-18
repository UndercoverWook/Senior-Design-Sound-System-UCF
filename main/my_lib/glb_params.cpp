/*
 * glb_params.cpp
 *
 *  Created on: Mar 30, 2026
 *      Author: matia
 */

#include "glb_params.h"


gptimer_handle_t     sync_timer    = NULL;
spi_device_handle_t  spi_hdl       = NULL;
i2s_chan_handle_t    mcu_tx        = NULL;
i2s_chan_handle_t    mcu_rx        = NULL;
wave_reader_handle_t wav_hdl       = NULL;
TaskHandle_t         bt_task       = NULL;
TaskHandle_t         usb_task      = NULL;
RingbufHandle_t      audio_ringbuf = NULL;
EventGroupHandle_t   sync_tasks    = NULL;

float cal_values[256][2]    = {};
bool activate_eq = false;
volatile bool wav_playback_active = false;
volatile bool calibration_in_progress = false;
float eq_freqs[EQ_BANDS] = {60.0f, 125.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};
float app_sliders[EQ_BANDS] = {0};
float eq_w[EQ_BANDS][2] = {{0}};
float sub_lpf_w[2] = {0};
float mid_hpf_w[2] = {0};
float eq_coeffs[EQ_BANDS][5] = {{0}};
float lpf_coeffs[5] = {0};
float hpf_coeffs[5] = {0};
bool usb_running = false;
volatile bool flush_required = false;

const char *STORAGE_TAG  = "File System";
const char *SPI_TAG      = "SPI Configuration";
const char *I2S_TAG      = "I2S Configuration";
const char *MIC_TAG      = "EMM6 Calibration";
const char *TEST_TAG     = "Test Signal Playback";
const char *SAMPLING_TAG = "ADC Sampling";
const char *WAV_TAG      = "WAV Test";
const char *EQ_TAG       = "Auto EQ";
const char *BM83_TAG     = "BM83 UART";
const char *USB_TAG      = "USB";