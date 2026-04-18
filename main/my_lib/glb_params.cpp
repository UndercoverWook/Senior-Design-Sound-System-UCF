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

float cal_values[256][2] = {};
bool activate_eq = false;
float eq_freqs[EQ_BANDS] = {60, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
float app_sliders[EQ_BANDS] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
extern float eq_w[EQ_BANDS][2] = {};
extern float sub_lpf_w[2] = {}; // State for Subwoofer (Left)
extern float mid_hpf_w[2] = {}; // State for Mids/Highs (Right)
extern float eq_coeffs[EQ_BANDS][5] = {}; 
extern float lpf_coeffs[5] = {};
extern float hpf_coeffs[5] = {};
extern bool usb_running = false;


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