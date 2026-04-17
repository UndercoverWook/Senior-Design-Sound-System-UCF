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

    //xTaskCreatePinnedToCore(vBT_playback_task, "BT Playback", STACK_DEPTH, NULL, configMAX_PRIORITIES - 1, NULL, CORE1);
    xTaskCreate(vUSB_playback_task, "USB Playback", STACK_DEPTH, NULL, 5, &usb_task); // Give USB less priority

    //play_and_sample();
}