/*
 * auto_eq_help.cpp
 *
 *  Created on: Apr 6, 2026
 *      Author: Matias Segura
 *
 *	This .cpp file contains all help functions to implement the Auto EQ algorithm, including:
 *	- File reading and interpolation for EMM6 calibration
 *	- FFT computation and magnitude calculation
 *	- Wiener deconvolution
 *  - FIR filter design from correction curve
 */

#include <math.h>
#include "auto_eq_help.h"
#include "help_functions.h"
#include "glb_params.h"
#include "ble_control.h"

static const int CAL_NUM_POINTS = 256;
static const float CAL_SENSITIVITY_1KHZ = -38.1f;  // dB re 1V/Pa at 1kHz

static void calculate_band_gains_from_H(float *H, int fft_size, float sample_rate, float *out_gains)
{
    for (int b = 0; b < EQ_BANDS; b++) {
        float low_f = eq_freqs[b] * 0.707f;
        float high_f = eq_freqs[b] * 1.414f;
        float sum_mag = 0.0f;
        int count = 0;
        for (int i = 0; i < fft_size / 2; i++) {
            float bin_freq = (float)i * sample_rate / fft_size;
            if (bin_freq >= low_f && bin_freq <= high_f) {
                float re = H[i * 2];
                float im = H[i * 2 + 1];
                sum_mag += sqrtf(re * re + im * im);
                count++;
            }
        }
        float gain_db = 0.0f;
        if (count > 0) {
            float avg_mag = sum_mag / (float)count;
            gain_db = -20.0f * log10f(avg_mag + 1e-6f);
        }
        if (gain_db > 6.0f) gain_db = 6.0f;
        if (gain_db < -6.0f) gain_db = -6.0f;
        out_gains[b] = gain_db;
    }
}

void show_FFT(float *fft_avg, int num_bins, float sample_rate)
{
    if (!fft_avg) {
        ESP_LOGE(EQ_TAG, "show_FFT: null input buffer");
        return;
    }

    for (int i = 0; i < num_bins; i++) {
        float freq_hz = (float)i * sample_rate / FFT_SIZE;

        float real    = fft_avg[i * 2 + 0];          // Real part of bin i
        float imag    = fft_avg[i * 2 + 1];          // Imaginary part of bin i
        float mag     = sqrtf(real * real + imag * imag);  // True magnitude

        float mag_db  = 20.0f * log10f(mag + 1e-9f); // Now always >= 0, no NaN

        ESP_LOGI(EQ_TAG, "Frequency: %8.2f Hz  |  Magnitude: %.4f dB", freq_hz, mag_db);
    }
}

void correction_ifft(float* correction_curve, int n)
{
    // Flip the imaginary part:
    for (int i = 0; i < n; i++) {
        correction_curve[i * 2 + 1] = -correction_curve[i * 2 + 1];
    }

    // Re-take FFT (serves as IFFT)
    dsps_fft2r_fc32_aes3(correction_curve, n);
    dsps_bit_rev_fc32(correction_curve, n);

    // Conjugate the result and apply scaling (1/N)
    float scale = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
        correction_curve[i * 2 + 1] = -correction_curve[i * 2 + 1];
        correction_curve[i * 2 + 0] *= scale;
        correction_curve[i * 2 + 1] *= scale;
    }
}

float* calculate_correction_curve(float *H, int n) 
{
    float *correction = (float *)heap_caps_malloc((size_t)(2 * n) * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float max_gain = 4.0f; // +12dB max boost

    if (correction == NULL || H == NULL) {
        if (correction != NULL) free(correction);
        return NULL;
    }

    for (int i = 0; i < n; i++) {
        float re = H[i * 2 + 0];
        float im = H[i * 2 + 1];
        float mag_sq = re * re + im * im + 1e-9f;
        
        // 1) Invert the complex number: 1/Z = conj(Z) / |Z|^2
        float inv_re = re / mag_sq;
        float inv_im = -im / mag_sq;

        // 2) Cap the Gain based on Magnitude
        float inv_mag = sqrtf(inv_re * inv_re + inv_im * inv_im);
        if (inv_mag > max_gain) {
            float scale = max_gain / inv_mag;
            inv_re *= scale;
            inv_im *= scale;
        }

        correction[i * 2 + 0] = inv_re;
        correction[i * 2 + 1] = inv_im;
    }
    
    return correction;
}

float* compute_wiener_deconvolution(float *X, float *Y, int n) 
{
   const float reg_factor = 0.01f;

    if (X == NULL || Y == NULL) {
        return NULL;
    }

    float *H = (float *)heap_caps_malloc((size_t)(2 * n) * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (H == NULL) {
        return NULL;
    }

    for (int i = 0; i < n * 2; i += 2) {
        float x_re = X[i],     x_im = X[i+1];
        float y_re = Y[i],     y_im = Y[i+1];

        float x_mag_sq = x_re*x_re + x_im*x_im;
        float divisor  = x_mag_sq + reg_factor;

        H[i]   = (y_re * x_re + y_im * x_im) / divisor;
        H[i+1] = (y_im * x_re - y_re * x_im) / divisor;
    }
    return H;
}
    
float apply_emm6_calibration(float freq_hz)
{
    // Clamp to calibration range
    if (freq_hz <= cal_values[0][0])
        return cal_values[0][1];
    if (freq_hz >= cal_values[CAL_NUM_POINTS - 1][0])
        return cal_values[CAL_NUM_POINTS - 1][1];

    // Find the two surrounding calibration points
    int lo = 0;
    int hi = CAL_NUM_POINTS - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (cal_values[mid][0] <= freq_hz)
            lo = mid;
        else
            hi = mid;
    }

    // Linear interpolation between lo and hi
    float t = (freq_hz - cal_values[lo][0]) / (cal_values[hi][0] - cal_values[lo][0]);
    return cal_values[lo][1] + t * (cal_values[hi][1] - cal_values[lo][1]);
}

void apply_calibration_to_fft(float *fft_acc, float sample_rate)
{
    for (int k = 0; k < NUM_BINS; k++) {
        float freq_hz = (float)k * sample_rate / FFT_SIZE;

        // Interpolate calibration correction in dB at this bin's frequency
        float cal_db = apply_emm6_calibration(freq_hz);

        // Convert dB correction to linear scale factor
        float scale = powf(10.0f, -cal_db / 20.0f);

        // Apply to complex bin — adjusts magnitude, preserves phase
        fft_acc[k*2 + 0] *= scale;  // real
        fft_acc[k*2 + 1] *= scale;  // imaginary
    }
}

float* compute_fft(uint16_t *samples, int num_samples, float sample_rate)
{ 
    // Allocate buffers for FFT processing and preprocessing
    float *y_cf       = (float *)heap_caps_malloc(FFT_SIZE * sizeof(float) * 2, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    float *wind_coeff = (float *)heap_caps_malloc(FFT_SIZE * sizeof(float),     MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    float *fft_acc    = (float *)heap_caps_malloc(FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_INTERNAL);

    if (!y_cf || !wind_coeff || !fft_acc) {
        ESP_LOGE(EQ_TAG, "FFT buffer alloc failed! y_cf=%p wind=%p fft_acc=%p", y_cf, wind_coeff, fft_acc);
        free(y_cf); free(wind_coeff); free(fft_acc);
        return NULL;
    }

    // Zero-init the accumulator (complex, so FFT_SIZE * 2)
    memset(fft_acc, 0, FFT_SIZE * sizeof(float) * 2);

    // Get Hann window coefficients
    dsps_wind_hann_f32(wind_coeff, FFT_SIZE);

    int hop        = NUM_BINS;
    int num_chunks = (num_samples - FFT_SIZE) / hop;

    for (int i = 0; i < num_chunks; i++) {
        uint16_t *chunk_start = samples + i * hop;

        for (int j = 0; j < FFT_SIZE; j++) {
           // int16_t signed_sample = (int16_t)chunk_start[j];
            y_cf[j*2 + 0] = ((chunk_start[j] - 32768.0f) / 32768.0f) * wind_coeff[j]; // Normalize for 16-bit (1.5V bias)
            y_cf[j*2 + 1] = 0.0f;                                       // Real signal so imaginary part is zero
        }

        // Take FFT and bit reverse the result (required for ESP-DSP lib)
        dsps_fft2r_fc32_aes3(y_cf, FFT_SIZE);
        dsps_bit_rev_fc32(y_cf, FFT_SIZE);

        // Accumulate raw complex results
        for (int k = 0; k < FFT_SIZE; k++) {
            fft_acc[k*2 + 0] += y_cf[k*2 + 0];  // real
            fft_acc[k*2 + 1] += y_cf[k*2 + 1];  // imaginary
        }
    }

    // Average across all chunks
    if (num_chunks > 0) {
        for (int k = 0; k < FFT_SIZE * 2; k++) {
            fft_acc[k] /= num_chunks;
        }
    }

    free(y_cf);
    free(wind_coeff);

    return fft_acc;
}

void normalize_taps(float* taps)
{
    // Calculate the sum of all taps
    float sum = 0;
    for (int i = 0; i < FFT_SIZE; i++) {
        sum += taps[i];
    }

    // Normalize to unity gain (DC = 0dB)
    if (fabsf(sum) > 1e-9f) {
        float norm_factor = 1.0f / sum;
        for (int i = 0; i < FFT_SIZE; i++) {
            taps[i] *= norm_factor;
        }
    }
}

float* run_Auto_EQ_algorithm(uint16_t* samples, float actual_freq)
{   
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE("FFT", "Failed to initialize FFT, ERROR: %s", esp_err_to_name(err));
        free(samples);
        return NULL;
    } else {
        ESP_LOGI(EQ_TAG, "FFT Initialized!");
    }

    float *wav_fft = wav_to_fft();
    emm6_file_to_arr();

    float *sample_fft = compute_fft(samples, N_SAMPLES, actual_freq);
    free(samples);
    samples = NULL;

    if (wav_fft == NULL || sample_fft == NULL) {
        ESP_LOGE(EQ_TAG, "FFT generation failed: wav_fft=%p sample_fft=%p", wav_fft, sample_fft);
        if (wav_fft) free(wav_fft);
        if (sample_fft) free(sample_fft);
        dsps_fft2r_deinit_fc32();
        return NULL;
    }

    apply_calibration_to_fft(sample_fft, actual_freq);
    ble_publish_fft_bins_from_complex(sample_fft, actual_freq);

    float *H = compute_wiener_deconvolution(wav_fft, sample_fft, FFT_SIZE);
    free(wav_fft);
    free(sample_fft);

    if (H == NULL) {
        ESP_LOGE(EQ_TAG, "Wiener deconvolution failed");
        dsps_fft2r_deinit_fc32();
        return NULL;
    }

    float *band_gains = (float *)heap_caps_calloc(EQ_BANDS, sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (band_gains == NULL) {
        free(H);
        dsps_fft2r_deinit_fc32();
        return NULL;
    }

    calculate_band_gains_from_H(H, FFT_SIZE, SAMPLE_RATE, band_gains);
    free(H);
    dsps_fft2r_deinit_fc32();
    return band_gains;
}
