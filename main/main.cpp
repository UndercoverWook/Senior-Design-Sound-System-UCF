#include <stdio.h>
#include "my_lib/auto_eq_help.h"
#include "my_lib/config_functions.h"
#include "my_lib/glb_params.h"
#include "my_lib/help_functions.h"
#include "my_lib/usb_device.h"
#include "my_lib/my_tasks.h"


extern "C" void app_main(void)
{
    configure_psram();
    configure_spiffs();

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
