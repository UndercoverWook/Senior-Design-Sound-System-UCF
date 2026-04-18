/*
 * help_functions.cpp
 *
 *  Created on: Mar 21, 2026
 *      Author: matia
 */

#include "help_functions.h"
#include "glb_params.h"
#include "auto_eq_help.h"
#include "my_tasks.h"
#include "dsps_biquad_gen.h"
#include "dsps_biquad.h"
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

float* wav_to_fft()
{
    uint16_t* samples = (uint16_t*)heap_caps_malloc(N_SAMPLES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    int count = load_wav_to_array("/storage/48k_4sec_sweep.wav", samples, N_SAMPLES);
    float* wav_fft = compute_fft(samples, count, SAMPLE_RATE, true);
    free(samples);

    return wav_fft;
}

void play_and_sample()
{
    sync_tasks = xEventGroupCreate();
    xTaskCreatePinnedToCore(vSample_task, "ADC Sampling", STACK_DEPTH, NULL, configMAX_PRIORITIES - 1, NULL, CORE1);
    xTaskCreatePinnedToCore(vPlay_WAV_task, "WAV Playback", STACK_DEPTH * 2, NULL, configMAX_PRIORITIES - 1, NULL, CORE0);    
    xEventGroupWaitBits(sync_tasks, ALL_TASKS_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
    vEventGroupDelete(sync_tasks);
}

void swap_bytes_16bit(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t tmp = buf[i];
        buf[i]      = buf[i + 1];
        buf[i + 1]  = tmp;
    }
}

esp_err_t my_dsps_biquad_gen_peakingEQ_f32(float *coeffs, float f, float gain_db, float qFactor)
{
    // 1. Safety check for Q
    if (qFactor <= 0.0001f) qFactor = 0.0001f;

    // 2. Calculate the Gain multiplier (A)
    float A = powf(10, gain_db / 40.0f);
    float w0 = 2 * M_PI * f;
    float c = cosf(w0);
    float s = sinf(w0);
    float alpha = s / (2.0f * qFactor);

    // 3. Calculate raw coefficients
    float b0 = 1 + (alpha * A);
    float b1 = -2 * c;
    float b2 = 1 - (alpha * A);
    float a0 = 1 + (alpha / A);
    float a1 = -2 * c;
    float a2 = 1 - (alpha / A);

    // 4. Normalize by a0 so a0 is effectively 1.0
    coeffs[0] = b0 / a0;
    coeffs[1] = b1 / a0;
    coeffs[2] = b2 / a0;
    coeffs[3] = a1 / a0;
    coeffs[4] = a2 / a0;

    return ESP_OK;
}

