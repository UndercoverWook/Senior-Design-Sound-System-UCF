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

static const int CAL_NUM_POINTS = 256;
static const float CAL_SENSITIVITY_1KHZ = -38.1f;  // dB re 1V/Pa at 1kHz

void show_FFT(float *fft_avg, int num_bins, float sample_rate)
{
    if (!fft_avg) {
        ESP_LOGE(EQ_TAG, "show_FFT: null input buffer");
        return;
    }

    for (int i = 0; i < num_bins; i++) {
        float freq_hz = (float)i * sample_rate / FFT_SIZE;
        float mag_db  = 20.0f * log10f(fft_avg[i] + 1e-9f);

        ESP_LOGI(EQ_TAG, "Frequency: %8.2f Hz  |  Magnitude: %.4f dB", freq_hz, mag_db);
    }
}

int load_wav_to_array(const char* filename, uint16_t* samples, int max_samples)
{
    wav_hdl = wave_reader_open(filename);

    if (wav_hdl == NULL) {
        ESP_LOGE(WAV_TAG, "Unable to open file: %s", filename);
        return -1;
    }

    uint8_t* buff = (uint8_t*)calloc(1, BUFFER_BYTES);
    if (buff == NULL) {
        ESP_LOGE(WAV_TAG, "Failed to allocate buffer");
        wave_reader_close(wav_hdl);
        return -1;
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

    ESP_LOGI(WAV_TAG, "Loaded %d samples from %s", sample_count, filename);
    return sample_count;  // Return the number of samples actually read
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
    float *correction = (float *)heap_caps_malloc(2 * n * sizeof(float), MALLOC_CAP_8BIT);
    float max_gain = 4.0f; // +12dB max boost

    for (int i = 0; i < n; i++) {
        float re = H[i*2 + 0];
        float im = H[i*2 + 1];
        float mag_sq = re*re + im*im + 1e-9f;
        
        // 1) Invert the complex number: 1/Z = conj(Z) / |Z|^2
        float inv_re = re / mag_sq;
        float inv_im = -im / mag_sq;

        // 2) Cap the Gain based on Magnitude
        float inv_mag = sqrtf(inv_re*inv_re + inv_im*inv_im);
        if (inv_mag > max_gain) {
            float scale = max_gain / inv_mag;
            inv_re *= scale;
            inv_im *= scale;
        }

        correction[i*2 + 0] = inv_re;
        correction[i*2 + 1] = inv_im;
    }
    free(H);

    return correction;
}

float* compute_wiener_deconvolution(float *X, float *Y, int n) 
{
    float reg_factor = 0.01f;
    
    for (int i = 0; i < n; i += 2) {
        float x_re = X[i],     x_im = X[i+1];
        float y_re = Y[i],     y_im = Y[i+1];

        float x_mag_sq = x_re*x_re + x_im*x_im;
        float divisor  = x_mag_sq + reg_factor;

        float h_re = (y_re*x_re + y_im*x_im) / divisor;
        float h_im = (y_im*x_re - y_re*x_im) / divisor;

        Y[i]   = h_re;
        Y[i+1] = h_im;
    }
    free(X);

    return Y;
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
        float scale = powf(10.0f, cal_db / 20.0f);

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
    float *fft_acc    = (float *)heap_caps_malloc(FFT_SIZE * sizeof(float) * 2, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

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
            y_cf[j*2 + 0] = ((chunk_start[j] - 32768.0f) / 32768.0f) * wind_coeff[j]; // Normalize for 16-bit (1.5V bias)
            y_cf[j*2 + 1] = 0.0f;                                                     // Real signal so imaginary part is zero
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
    
    free(samples);

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
    // Initialize FFT tables (must be done before calling any FFT functions) and compute FFT
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE("FFT", "Failed to initialize FFT, ERROR: %s", esp_err_to_name(err));
        return;
    }
    // Make wav file to FFT
    wav_to_fft();

    // Load calibration file to array before applying calibration
    emm6_file_to_arr();

    // Take FFT of sampled data and load magnitudes from WAV file (stored in SPIFFS)
    float *wav_fft = load_fft_cache(FFT_SIZE);
    float *sample_fft = compute_fft(samples, N_SAMPLES, actual_freq);   // Apply calibration to get "true" magnitudes

    // Apply calibration to samples FFT
    apply_calibration_to_fft(sample_fft, actual_freq);
    
    // Compute Wiener deconvolution on the magnitudes
    float *H = compute_wiener_deconvolution(wav_fft, sample_fft, FFT_SIZE);

    // Calculate correction curve based on Target Curve (1.0 across all bins)
    // and the computed H (system response) using Wiener deconvolution
    float *correction_curve = calculate_correction_curve(H, FFT_SIZE);

    // Turn correction curve into FIR filter coefficients (IFFT)
    correction_ifft(correction_curve, FFT_SIZE);

    float *final_taps = (float *)heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_INTERNAL);
    float *window = (float *)malloc(FFT_SIZE * sizeof(float));
    dsps_wind_blackman_f32(window, FFT_SIZE);

    // This moves the impulse from t=0 to the center of the buffer (Circular Shift)
    for (int i = 0; i < FFT_SIZE; i++) {
        int shifted_idx = (i + (FFT_SIZE / 2)) % FFT_SIZE;
        float raw_tap = correction_curve[shifted_idx * 2];
        final_taps[i] = raw_tap * window[i];
    }

    free(window);
    free(correction_curve);
    
    // Normalize final taps for FIR design
    normalize_taps(final_taps);

    return final_taps;
}