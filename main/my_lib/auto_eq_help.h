#ifndef MAIN_MY_FUNCTIONS_AUTO_EQ_HELP_H_
#define MAIN_MY_FUNCTIONS_AUTO_EQ_HELP_H_

#pragma once
#include "glb_params.h"

#ifdef __cplusplus
extern "C" {
#endif

// Function to print FFT results
void show_FFT(float *y_cf, int n, float sample_rate);

// Function to run the AutoEQ algorithm on the sampled data
float* run_Auto_EQ_algorithm(uint16_t* samples, float actual_freq);

// Function to compute the FFT and print the calibrated magnitude values
float* compute_fft(uint16_t *samples, int num_samples, float sample_rate);

// Function to apply EMM6 calibration to the raw magnitude values by interpolation
float apply_emm6_calibration(float freq_hz);

// Apply calibration to full FFT result
void apply_calibration_to_fft(float *fft_acc, float sample_rate);

// Function to compute Wiener Deconvolution in frequency domain
float* compute_wiener_deconvolution(float *X, float *Y, int n);

// Function to calculate correction curve from target curve and IR (Frequency domain division)
float* calculate_correction_curve(float *ir_freq_domain, int n);

// Function to transform correction curve to Time-Domain IR
void correction_ifft(float* correction_curve, int n);

// Normalize final taps
void normalize_taps(float* taps);


#ifdef __cplusplus
}
#endif

#endif /* MAIN_MY_FUNCTIONS_AUTO_EQ_HELP_H_ */