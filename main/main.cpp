#include <stdio.h>
#include "my_lib/auto_eq_help.h"
#include "my_lib/ble_control.h"
#include "my_lib/config_functions.h"
#include "my_lib/glb_params.h"
#include "my_lib/help_functions.h"
#include "my_lib/my_usb_device.h"
#include "my_lib/my_tasks.h"

extern "C" void app_main(void)
{
    configure_psram();
    configure_spiffs();
    reconfigure_wdt();
    ble_control_init();

    xTaskCreatePinnedToCore(vBT_playback_task, "BT Playback", 8192, NULL, 5, &bt_task, CORE1);
    // Leave USB playback task disabled during BLE/Auto-EQ testing so it does not occupy I2S0.
    // xTaskCreatePinnedToCore(vUSB_playback_task, "USB Playback", 8192, NULL, 10, &usb_task, CORE0);

    // Calibration must only start from the AUTO_EQ_START BLE command path.
    // Do NOT call play_and_sample() here.
    }
