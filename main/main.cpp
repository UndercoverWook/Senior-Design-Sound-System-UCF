#include <stdio.h>
#include "my_lib/auto_eq_help.h"
#include "my_lib/config_functions.h"
#include "my_lib/glb_params.h"
#include "my_lib/help_functions.h"
#include "my_lib/my_usb_device.h"
#include "my_lib/my_tasks.h"

extern "C" void app_main(void)
{
    // On Start:
    configure_psram();
    configure_spiffs();
    reconfigure_wdt();
    configure_i2s();

    xTaskCreate(vUSB_playback_task, "USB Playback", STACK_DEPTH, NULL, 5, &usb_task);

    // play_and_sample();

    // dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    // float *wav_fft = wav_to_fft();
    // show_FFT(wav_fft, NUM_BINS, SAMPLE_RATE);
}