/*
 * help_functions.cpp
 *
 *  Created on: Mar 21, 2026
 *      Author: matia
 */

#include "help_functions.h"
#include "glb_params.h"
#include <math.h>

void bm83_wakeup ()
{
	gpio_set_direction(BT_WAKE, GPIO_MODE_OUTPUT);
	gpio_set_level(BT_WAKE, 1);   // Pull MFB high
	vTaskDelay(pdMS_TO_TICKS(10));  // Hold for ~5 ms (safely above 2 ms minimum)
	gpio_set_level(BT_WAKE, 0);   // Release
	vTaskDelay(pdMS_TO_TICKS(2));   // Brief settle before sending UART
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