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

    xTaskCreatePinnedToCore(vBT_playback_task, "BT Playback", 8192, NULL, 5, &bt_task, CORE1);

    vTaskDelay(pdMS_TO_TICKS(20000));   // wait 20 seconds and then start calibration

    play_and_sample();
    // xTaskCreatePinnedToCore(vUSB_playback_task, "USB Playback", 8192, NULL, 10, &usb_task, CORE0); // Give USB less priority
}