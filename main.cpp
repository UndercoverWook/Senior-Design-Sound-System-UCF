#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define BM83_MFB_PIN GPIO_NUM_48

extern "C" void app_main(void)
{
    gpio_reset_pin(BM83_MFB_PIN);
    gpio_set_direction(BM83_MFB_PIN, GPIO_MODE_OUTPUT);

    gpio_set_level(BM83_MFB_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level(BM83_MFB_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));   // longer test pulse

    gpio_set_level(BM83_MFB_PIN, 0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
