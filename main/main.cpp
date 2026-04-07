#include <stdio.h>
#include "my_lib/auto_eq_help.h"
#include "my_lib/config_functions.h"
#include "my_lib/glb_params.h"
#include "my_lib/help_functions.h"
#include "my_lib/usb_device.h"
#include "my_lib/my_tasks.h"


extern "C" void app_main(void)
{
    configure_spiffs();
    configure_spi();
    configure_psram();
    reconfigure_wdt();

    uint16_t *wav_samples = (uint16_t *)heap_caps_malloc(N_SAMPLES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    load_wav_to_array("/storage/44k_full_sweep.wav", wav_samples, N_SAMPLES);
    float *wav_mags = compute_fft(wav_samples, N_SAMPLES, SAMPLE_RATE);

    for (int i = 0; i < N_SAMPLES / 2; i++) {
        printf("Bin %d: Magnitude = %f\n", i, wav_mags[i]);
    }

    //xTaskCreatePinnedToCore(vSample_task, "Sample Task", 8192, NULL, configMAX_PRIORITIES - 1, &task1_hdl, CORE1);
}
