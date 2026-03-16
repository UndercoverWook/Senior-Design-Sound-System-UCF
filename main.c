#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/uac_host.h"

#include "aoa_audio.h"

static const char *TAG = "usb_android_audio";

#define USB_HOST_TASK_PRIORITY   5
#define UAC_TASK_PRIORITY        5
#define AOA_TASK_PRIORITY        4

/*
 * -------------------------
 * CHANGE THESE FOR YOUR PCB
 * -------------------------
 * This file assumes your board has an external I2S DAC/codec that feeds your amp.
 * If your board uses different pins or needs MCLK, update them here.
 */
#define I2S_BCLK_GPIO        GPIO_NUM_12
#define I2S_WS_GPIO          GPIO_NUM_11
#define I2S_DOUT_GPIO        GPIO_NUM_13
#define I2S_MCLK_GPIO        I2S_GPIO_UNUSED

/*
 * If your USB-A 5V VBUS is switched by a GPIO, set that GPIO here.
 * Otherwise leave it as GPIO_NUM_NC.
 */
#define USB_VBUS_EN_GPIO     GPIO_NUM_NC

/* Android AOA audio is fixed to 44.1 kHz, 16-bit, stereo */
#define PHONE_SAMPLE_RATE    44100
#define PHONE_BITS           16
#define PHONE_CHANNELS       2

#define UAC_RX_BUFFER_BYTES  8192

static QueueHandle_t s_event_queue = NULL;
static TaskHandle_t s_uac_task_handle = NULL;
static TaskHandle_t s_aoa_task_handle = NULL;

static uac_host_device_handle_t s_phone_uac_handle = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static uint8_t *s_uac_rx_buf = NULL;

/* -----------------------------
 * UAC event queue definitions
 * ----------------------------- */
typedef enum {
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

typedef struct {
    event_group_t event_group;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_handle_t handle;
            uac_host_device_event_t event;
            void *arg;
        } device_evt;
    };
} app_event_t;

/* -----------------------------
 * AOA raw-host client state
 * ----------------------------- */
#define AOA_ACTION_OPEN_DEV   (1U << 0)
#define AOA_ACTION_DEV_GONE   (1U << 1)

typedef struct {
    volatile uint32_t actions;
    uint8_t dev_addr;
    usb_host_client_handle_t client_hdl;
} aoa_client_ctrl_t;

/* -----------------------------
 * Hardware helpers
 * ----------------------------- */
static void enable_usb_vbus_if_needed(void)
{
    if (USB_VBUS_EN_GPIO != GPIO_NUM_NC) {
        gpio_reset_pin(USB_VBUS_EN_GPIO);
        gpio_set_direction(USB_VBUS_EN_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(USB_VBUS_EN_GPIO, 1);
        ESP_LOGI(TAG, "USB VBUS enabled on GPIO %d", USB_VBUS_EN_GPIO);
    }
}

static void init_i2s_output(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PHONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));

    ESP_LOGI(TAG,
             "I2S TX ready: BCLK=%d WS=%d DOUT=%d MCLK=%d",
             I2S_BCLK_GPIO,
             I2S_WS_GPIO,
             I2S_DOUT_GPIO,
             I2S_MCLK_GPIO);
}

static esp_err_t i2s_write_all(const uint8_t *data, size_t len)
{
    if (s_i2s_tx == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t total_written = 0;
    while (total_written < len) {
        size_t just_written = 0;
        esp_err_t err = i2s_channel_write(s_i2s_tx,
                                          data + total_written,
                                          len - total_written,
                                          &just_written,
                                          portMAX_DELAY);
        if (err != ESP_OK) {
            return err;
        }
        total_written += just_written;
    }

    return ESP_OK;
}

/* -----------------------------
 * UAC callbacks
 * ----------------------------- */
static void uac_device_callback(uac_host_device_handle_t uac_device_handle,
                                const uac_host_device_event_t event,
                                void *arg)
{
    app_event_t evt = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg,
    };
    xQueueSend(s_event_queue, &evt, 0);
}

static void uac_host_lib_callback(uint8_t addr,
                                  uint8_t iface_num,
                                  const uac_host_driver_event_t event,
                                  void *arg)
{
    app_event_t evt = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg,
    };
    xQueueSend(s_event_queue, &evt, 0);
}

/* -----------------------------
 * USB host daemon task
 * ----------------------------- */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host library installed");

    if (s_aoa_task_handle) {
        xTaskNotifyGive(s_aoa_task_handle);
    }
    if (s_uac_task_handle) {
        xTaskNotifyGive(s_uac_task_handle);
    }

    while (true) {
        uint32_t event_flags = 0;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "USB host has no clients");
        }
    }
}

/* -----------------------------
 * Raw USB client for AOA switch
 * ----------------------------- */
static void aoa_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    aoa_client_ctrl_t *ctrl = (aoa_client_ctrl_t *)arg;

    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ctrl->actions |= AOA_ACTION_OPEN_DEV;
        ctrl->dev_addr = event_msg->new_dev.address;
        break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ctrl->actions |= AOA_ACTION_DEV_GONE;
        break;

    default:
        break;
    }
}

static void aoa_probe_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    aoa_client_ctrl_t ctrl = {0};
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = aoa_client_event_cb,
            .callback_arg = &ctrl,
        },
    };

    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &ctrl.client_hdl));
    ESP_LOGI(TAG, "AOA probe client registered");

    while (true) {
        esp_err_t err = usb_host_client_handle_events(ctrl.client_hdl, portMAX_DELAY);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "AOA client handle events failed: %s", esp_err_to_name(err));
            continue;
        }

        if (ctrl.actions & AOA_ACTION_OPEN_DEV) {
            ctrl.actions &= ~AOA_ACTION_OPEN_DEV;

            usb_device_handle_t dev_hdl = NULL;
            err = usb_host_device_open(ctrl.client_hdl, ctrl.dev_addr, &dev_hdl);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "usb_host_device_open(%u) failed: %s", ctrl.dev_addr, esp_err_to_name(err));
                continue;
            }

            const usb_device_desc_t *dev_desc = NULL;
            err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
            if (err == ESP_OK && dev_desc != NULL) {
                ESP_LOGI(TAG,
                         "USB device connected: addr=%u VID=0x%04X PID=0x%04X",
                         ctrl.dev_addr,
                         dev_desc->idVendor,
                         dev_desc->idProduct);

                if (aoa_is_audio_mode_device(dev_desc->idVendor, dev_desc->idProduct)) {
                    ESP_LOGI(TAG, "Device is already in AOA audio mode");
                } else {
                    err = aoa_try_enable_audio_mode(ctrl.client_hdl, dev_hdl);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "AOA audio mode requested; waiting for phone to re-enumerate");
                    } else {
                        ESP_LOGW(TAG, "AOA audio request not applied: %s", esp_err_to_name(err));
                    }
                }
            }

            ESP_ERROR_CHECK(usb_host_device_close(ctrl.client_hdl, dev_hdl));
        }

        if (ctrl.actions & AOA_ACTION_DEV_GONE) {
            ctrl.actions &= ~AOA_ACTION_DEV_GONE;
            ESP_LOGI(TAG, "USB device removed");
        }
    }
}

/* -----------------------------
 * UAC task: receive PCM from phone
 * ----------------------------- */
static void uac_lib_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL,
    };

    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC driver installed");

    app_event_t evt = {0};

    while (true) {
        if (!xQueueReceive(s_event_queue, &evt, portMAX_DELAY)) {
            continue;
        }

        if (evt.event_group == UAC_DRIVER_EVENT) {
            switch (evt.driver_evt.event) {
            case UAC_HOST_DRIVER_EVENT_RX_CONNECTED: {
                ESP_LOGI(TAG, "UAC source connected from device addr=%u iface=%u",
                         evt.driver_evt.addr,
                         evt.driver_evt.iface_num);

                if (s_phone_uac_handle != NULL) {
                    ESP_LOGW(TAG, "A UAC source is already active; ignoring new one");
                    break;
                }

                uac_host_device_handle_t uac_device_handle = NULL;
                const uac_host_device_config_t dev_config = {
                    .addr = evt.driver_evt.addr,
                    .iface_num = evt.driver_evt.iface_num,
                    .buffer_size = 19200,
                    .callback = uac_device_callback,
                    .callback_arg = NULL,
                };

                ESP_ERROR_CHECK(uac_host_device_open(&dev_config, &uac_device_handle));

                uac_host_dev_alt_param_t alt_params;
                ESP_ERROR_CHECK(uac_host_get_device_alt_param(uac_device_handle, 1, &alt_params));

                uac_host_stream_config_t stream_config = {
                    .channels = alt_params.channels,
                    .bit_resolution = alt_params.bit_resolution,
                    .sample_freq = alt_params.sample_freq[0],
                };

                ESP_LOGI(TAG,
                         "Phone audio stream: %" PRIu32 " Hz, %u bits, %u channels",
                         stream_config.sample_freq,
                         stream_config.bit_resolution,
                         stream_config.channels);

                if (stream_config.sample_freq != PHONE_SAMPLE_RATE ||
                    stream_config.bit_resolution != PHONE_BITS ||
                    stream_config.channels != PHONE_CHANNELS) {
                    ESP_LOGE(TAG,
                             "Unexpected phone audio format. Expected %d Hz / %d-bit / %d ch",
                             PHONE_SAMPLE_RATE,
                             PHONE_BITS,
                             PHONE_CHANNELS);
                    ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
                    break;
                }

                if (uac_host_device_start(uac_device_handle, &stream_config) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start UAC source stream");
                    ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
                    break;
                }

                if (s_uac_rx_buf == NULL) {
                    s_uac_rx_buf = (uint8_t *)malloc(UAC_RX_BUFFER_BYTES);
                    if (s_uac_rx_buf == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate RX buffer");
                        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
                        break;
                    }
                }

                s_phone_uac_handle = uac_device_handle;
                break;
            }

            case UAC_HOST_DRIVER_EVENT_TX_CONNECTED:
                ESP_LOGI(TAG, "Ignoring UAC TX device; this project only uses phone->board audio input");
                break;

            default:
                break;
            }
        } else if (evt.event_group == UAC_DEVICE_EVENT) {
            switch (evt.device_evt.event) {
            case UAC_HOST_DRIVER_EVENT_DISCONNECTED: {
                ESP_LOGI(TAG, "UAC device disconnected");
                uac_host_device_handle_t handle = evt.device_evt.handle;

                if (handle == s_phone_uac_handle) {
                    s_phone_uac_handle = NULL;
                }

                if (handle != NULL) {
                    ESP_ERROR_CHECK(uac_host_device_close(handle));
                }
                break;
            }

            case UAC_HOST_DEVICE_EVENT_RX_DONE:
                if (evt.device_evt.handle == s_phone_uac_handle && s_uac_rx_buf != NULL) {
                    uint32_t rx_size = 0;
                    esp_err_t err_read = uac_host_device_read(s_phone_uac_handle,
                                                              s_uac_rx_buf,
                                                              UAC_RX_BUFFER_BYTES,
                                                              &rx_size,
                                                              0);
                    if (err_read != ESP_OK) {
                        ESP_LOGE(TAG, "uac_host_device_read failed: %s", esp_err_to_name(err_read));
                        break;
                    }

                    if (rx_size > 0) {
                        esp_err_t err_i2s = i2s_write_all(s_uac_rx_buf, rx_size);
                        if (err_i2s != ESP_OK) {
                            ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err_i2s));
                        }
                    }
                }
                break;

            case UAC_HOST_DEVICE_EVENT_TX_DONE:
                break;

            case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                ESP_LOGW(TAG, "UAC transfer error");
                break;

            default:
                break;
            }
        }
    }
}

void app_main(void)
{
    enable_usb_vbus_if_needed();
    init_i2s_output();

    s_event_queue = xQueueCreate(16, sizeof(app_event_t));
    assert(s_event_queue != NULL);

    BaseType_t ok;

    ok = xTaskCreatePinnedToCore(uac_lib_task,
                                 "uac_task",
                                 4096,
                                 NULL,
                                 UAC_TASK_PRIORITY,
                                 &s_uac_task_handle,
                                 0);
    assert(ok == pdTRUE);

    ok = xTaskCreatePinnedToCore(aoa_probe_task,
                                 "aoa_task",
                                 4096,
                                 NULL,
                                 AOA_TASK_PRIORITY,
                                 &s_aoa_task_handle,
                                 0);
    assert(ok == pdTRUE);

    ok = xTaskCreatePinnedToCore(usb_lib_task,
                                 "usb_lib",
                                 4096,
                                 NULL,
                                 USB_HOST_TASK_PRIORITY,
                                 NULL,
                                 0);
    assert(ok == pdTRUE);
}
