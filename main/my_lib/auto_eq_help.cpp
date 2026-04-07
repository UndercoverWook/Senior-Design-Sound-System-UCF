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

#define WAV_FILE "storage/44k_full_sweep.wav"

void load_wav_to_array(const char* filename, uint16_t* samples, int max_samples)
{
    wav_hdl = wave_reader_open(filename);

    if (wav_hdl == NULL) {
        ESP_LOGE(WAV_FILE, "Unable to open file: %s", filename);
        return;
    }

    uint8_t* buff = (uint8_t*)calloc(1, BUFFER_BYTES);
    if (buff == NULL) {
        ESP_LOGE(WAV_FILE, "Failed to allocate buffer");
        wave_reader_close(wav_hdl);
        return;
    }

    size_t pos = 0;
    int sample_count = 0;

    while (sample_count < max_samples)
    {
        size_t bytes_read = wave_read_raw_data(wav_hdl, buff, pos, BUFFER_BYTES);

        if (bytes_read == 0) {
            break;  // End of file
        }

        pos += bytes_read;

        // Reinterpret the raw bytes as 16-bit samples.
        // Each uint16_t sample = 2 bytes, so iterate in steps of 2.
        for (size_t i = 0; i + 1 < bytes_read && sample_count < max_samples; i += 2)
        {
            // Little-endian: low byte first, high byte second (standard WAV format)
            samples[sample_count++] = (uint16_t)(buff[i] | (buff[i + 1] << 8));
        }
    }

    wave_reader_close(wav_hdl);
    free(buff);
    return;
}

float* compute_wiener_deconvolution(uint16_t *samples, int n) 
{
    uint16_t *input_signal = (uint16_t *)heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_SPIRAM);  // Allocate memory for input signal (copy of samples)
    load_wav_to_array(WAV_FILE, input_signal, n);  // Load the WAV file into the input signal array


    float reg_factor = 0.01f;
    float *X = compute_fft(input_signal, n, SAMPLE_RATE);    // Compute FFT of the input signal (X is modified in-place to contain the FFT output)
    float *Y = compute_fft(samples, n, SAMPLE_RATE);         // Compute FFT of the output signal (Y is modified in-place to contain the FFT output)

    for (int i = 0; i < n; i += 2) {
        float x_re = X[i],     x_im = X[i+1];
        float y_re = Y[i],     y_im = Y[i+1];

        float x_mag_sq = x_re*x_re + x_im*x_im;
        float divisor  = x_mag_sq + reg_factor;

        // H[k] = Y[k] / X[k]  =  Y[k] * conj(X[k]) / |X[k]|^2
        // (multiply numerator and denominator by conj(X) to make denominator real)
        float h_re = (y_re*x_re + y_im*x_im) / divisor;
        float h_im = (y_im*x_re - y_re*x_im) / divisor;

        Y[i]   = h_re;
        Y[i+1] = h_im;
    }

    return Y;
}

float apply_emm6_calibration(float freq_hz, float raw_db) 
{

    // Linear interpolation between nearest two points
    for (int i = 0; i < 256 - 1; i++) {
        if (freq_hz >= cal_values[i][0] && freq_hz < cal_values[i+1][0]) {
            float t = (freq_hz - cal_values[i][0]) / (cal_values[i+1][0] - cal_values[i][0]);
            float correction = cal_values[i][1] + t * (cal_values[i+1][1] - cal_values[i][1]);
            return raw_db + correction;
        }
    }
    return raw_db;
}

float* compute_fft(uint16_t *samples, int num_samples, float sample_rate)
{ 
    float *mag        = (float *)heap_caps_malloc(NUM_BINS * sizeof(float), MALLOC_CAP_SPIRAM);
    float *y_cf       = (float *)heap_caps_malloc(FFT_SIZE * sizeof(float) * 2, MALLOC_CAP_DEFAULT);
    float *wind_coeff = (float *)heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_DEFAULT);

    dsps_wind_hann_f32(wind_coeff, FFT_SIZE);
    memset(mag, 0, NUM_BINS * sizeof(float));

    int hop        = NUM_BINS;
    int num_chunks = (num_samples - FFT_SIZE) / hop;

    for (int i = 0; i < num_chunks; i++) {
        uint16_t *chunk_start = samples + i * hop;

        for (int j = 0; j < FFT_SIZE; j++) {
            y_cf[j*2 + 0] = ((chunk_start[j] - 32768.0f) / 32768.0f) * wind_coeff[j];
            y_cf[j*2 + 1] = 0.0f;
        }

        dsps_fft2r_fc32_aes3(y_cf, FFT_SIZE);
        dsps_bit_rev_fc32(y_cf, FFT_SIZE);

        for (int k = 0; k < NUM_BINS; k++) {
            float re = y_cf[k*2 + 0];
            float im = y_cf[k*2 + 1];
            mag[k] += sqrtf(re*re + im*im) / (FFT_SIZE / 2);
        }
    }

    for (int i = 0; i < NUM_BINS; i++) mag[i] /= num_chunks;

    free(y_cf);
    free(wind_coeff);

    for (int i = 1; i < NUM_BINS; i++) {
        float freq_hz = (float)i * sample_rate / FFT_SIZE;
        float mag_db = 20.0f * log10f(mag[i] + 1e-9f);
        float cal_db = apply_emm6_calibration(freq_hz, mag_db);
        mag[i] = cal_db;
    }
    return mag;
}

void run_Auto_EQ_algorithm(uint16_t* samples, float *actual_freq) 
{
    // 1) Wiener Deconvolution (returns a pointer to Transfer Function (Impulse Response) in frequency domain)
    // 2) Transfer function time domain conversion (IFFT, returns pointer to impulse response (time-domain))
    // 3) Apply Hann window to the impulse response
    // 4) Take FFT of the windowed impulse response (returns pointer of Impulse Response in frequency domain)
    // 5) Get Correction Curve from Target Curve and IR (Frequency domain division) 
    // 6) Design FIR filters from Correction Curve 

    emm6_file_to_arr();                             // Load calibration file to array before applying calibration
    compute_fft(samples, N_SAMPLES, *actual_freq);  // Compute the FFT of the sampled data

    compute_wiener_deconvolution(samples, N_SAMPLES);
}