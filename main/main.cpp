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
}