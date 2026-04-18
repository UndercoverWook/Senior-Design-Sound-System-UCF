/*
 * help_functions.h
 *
 *  Created on: Mar 21, 2026
 *      Author: matia
 */

#ifndef MAIN_MY_FUNCTIONS_HELP_FUNCTIONS_H_
#define MAIN_MY_FUNCTIONS_HELP_FUNCTIONS_H_

#pragma once

#include "glb_params.h"

#ifdef __cplusplus
extern "C" {
#endif

// Function to wake up the BM83 module
void bm83_tx_ind_init(void);

// Function to print the contents of a wave header struct
void print_wav(wave_header_t *wav_head);

// Function to read EMM6 calibration data from a file and store it in an array
void emm6_file_to_arr();

// Function to load FFT magnitudes from a cache file
float* load_fft_cache(int num_bins);

// Load FFT magnitudes, or if not cached, compute them from the WAV file and cache the results
float* wav_to_fft();

// Perform Phase 1 of the algorithm by creating a Sampling task and a Test Signal Playback task
void play_and_sample();

// Function to load the WAV file and convert it to an array of samples
int load_wav_to_array(const char* filename, uint16_t* samples, int max_samples);

// Swap bytes for I2S (from USB)
void swap_bytes_16bit(uint8_t *buf, size_t len);

// Overwrite ESP-DSP Peak EQ generation function
esp_err_t my_dsps_biquad_gen_peakingEQ_f32(float *coeffs, float f, float gain_db, float qFactor);


#ifdef __cplusplus
}
#endif

#endif /* MAIN_MY_FUNCTIONS_HELP_FUNCTIONS_H_ */
