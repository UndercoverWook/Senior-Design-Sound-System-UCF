#include <stdio.h>
#include "my_lib/auto_eq_help.h"
#include "my_lib/config_functions.h"
#include "my_lib/glb_params.h"
#include "my_lib/help_functions.h"
#include "my_lib/my_usb_device.h"
#include "my_lib/my_tasks.h"

void play_and_sample()
{
    sync_tasks = xEventGroupCreate();
    xTaskCreatePinnedToCore(vSample_task, "ADC Sampling", 8192, NULL, configMAX_PRIORITIES - 1, NULL, CORE0);
    xTaskCreatePinnedToCore(vPlay_WAV_task, "WAV Playback", 8192, NULL, configMAX_PRIORITIES - 1, NULL, CORE1);
}



extern "C" void app_main(void)
{
    // On Start:
    configure_psram();
    configure_spiffs();
    reconfigure_wdt();
}