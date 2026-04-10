/*
 * help_functions.cpp
 *
 *  Created on: Mar 21, 2026
 *      Author: matia
 */

#include "help_functions.h"
#include "glb_params.h"
#include "auto_eq_help.h"
#include <math.h>

void bm83_tx_ind_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MCU_WAKE),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,  // Active low, so pull up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}


void print_wav(wave_header_t *wav_head)
{
	ESP_LOGI(WAV_TAG, "Bytes per sample: %u", wav_head->bytes_per_sample);
	ESP_LOGI(WAV_TAG, "# of channels: %u", wav_head->n_channels);
	ESP_LOGI(WAV_TAG, "Sample Rate: %u", wav_head->sample_rate);
	ESP_LOGI(WAV_TAG, "Samples per Channel: %u", wav_head->samples_per_channel);
}

void emm6_file_to_arr()
{	
	FILE* fptr = fopen("/storage/EMM6_calibration.txt", "r");
	if (fptr == NULL) {
		ESP_LOGE(MIC_TAG, "Failed to open file.");
		return;
	}
		
	float frequency, magnitude;
		
	char line [15];						// To store the first line of the file
	fgets(line, 15, fptr);				// Consume the first line (Irrelevant)
	uint16_t i = 0;
		
	while(i < 256 && fscanf(fptr, "%f\t%f", &frequency, &magnitude) == 2)
	{
		cal_values[i][0] = frequency;
		cal_values[i][1] = magnitude;
		i++;
			
		if (i % 10 == 0)
			vTaskDelay(1);
	}
	fclose(fptr);
	
	return;
}// end of file_to_arr function

void save_fft_cache(float* mag, int num_bins)
{
    FILE* f = fopen(FFT_CACHE_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(WAV_TAG, "Failed to open cache file for writing");
        return;
    }
    fwrite(mag, sizeof(float), num_bins, f);
    fclose(f);
    ESP_LOGI(WAV_TAG, "FFT cache saved (%d bins)", num_bins);
}

float* load_fft_cache(int num_bins)
{
    FILE* f = fopen(FFT_CACHE_PATH, "rb");
    if (f == NULL) {
        return NULL;  // Cache doesn't exist yet
    }

    float* mag = (float*)heap_caps_malloc(num_bins * sizeof(float), MALLOC_CAP_DEFAULT);
    if (mag == NULL) {
        ESP_LOGE(WAV_TAG, "Failed to allocate mag buffer");
        fclose(f);
        return NULL;
    }

    size_t read = fread(mag, sizeof(float), num_bins, f);
    fclose(f);

    if (read != num_bins) {
        ESP_LOGE(WAV_TAG, "Cache file incomplete, expected %d bins, got %d", num_bins, (int)read);
        free(mag);
        return NULL;
    }

    ESP_LOGI(WAV_TAG, "FFT cache loaded (%d bins)", num_bins);
    return mag;
}

void wav_to_fft()
{
	float* mag = load_fft_cache(NUM_BINS);

    if (mag == NULL) {
        ESP_LOGI(WAV_TAG, "No cache found, running full FFT pipeline...");

        ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, FFT_SIZE));

        uint16_t* samples = (uint16_t*)heap_caps_malloc(N_SAMPLES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        int count = load_wav_to_array("/storage/44k_full_sweep.wav", samples, N_SAMPLES);

        mag = compute_fft(samples, count, 44100.0f, false);

        free(samples);
        dsps_fft2r_deinit_fc32();

        save_fft_cache(mag, NUM_BINS);
    } else {
        ESP_LOGI(WAV_TAG, "Loaded FFT results from cache, skipping pipeline");
    }
    free(mag);
}