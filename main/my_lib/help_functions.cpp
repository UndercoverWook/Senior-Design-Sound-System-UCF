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
	ESP_LOGI(WAV_FILE, "Bytes per sample: %u", wav_head->bytes_per_sample);
	ESP_LOGI(WAV_FILE, "# of channels: %u", wav_head->n_channels);
	ESP_LOGI(WAV_FILE, "Sample Rate: %u", wav_head->sample_rate);
	ESP_LOGI(WAV_FILE, "Samples per Channel: %u", wav_head->samples_per_channel);
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