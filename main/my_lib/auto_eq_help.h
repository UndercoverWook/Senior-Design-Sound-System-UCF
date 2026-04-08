#ifndef MAIN_MY_FUNCTIONS_AUTO_EQ_HELP_H_
#define MAIN_MY_FUNCTIONS_AUTO_EQ_HELP_H_

#pragma once
#include "glb_params.h"

#ifdef __cplusplus
extern "C" {
#endif

// Function to run the AutoEQ algorithm on the sampled data
void run_Auto_EQ_algorithm(uint16_t* samples, float *actual_freq);

// Function to compute the FFT and print the calibrated magnitude values
float* compute_fft(uint16_t *samples, int num_samples, float sample_rate, bool apply_calibration);

// Function to apply EMM6 calibration to the raw magnitude values
float apply_emm6_calibration(float freq_hz, float raw_db);

// Function to compute Wiener Deconvolution in frequency domain
float* compute_wiener_deconvolution(uint16_t *samples, int n);

// Function to load the WAV file and convert it to an array of samples
int load_wav_to_array(const char* filename, uint16_t* samples, int max_samples);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_MY_FUNCTIONS_AUTO_EQ_HELP_H_ */