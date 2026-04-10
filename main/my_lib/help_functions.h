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

// Function to save wav file FFT magnitudes to a cache file
void save_fft_cache(float* mag, int num_bins);

// Function to load FFT magnitudes from a cache file
float* load_fft_cache(int num_bins);

// Load FFT magnitudes, or if not cached, compute them from the WAV file and cache the results
void wav_to_fft();

#ifdef __cplusplus
}
#endif

#endif /* MAIN_MY_FUNCTIONS_HELP_FUNCTIONS_H_ */
