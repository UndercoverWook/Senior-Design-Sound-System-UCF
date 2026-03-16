#include "aoa_audio.h"

#include <string.h>
#include "esp_log.h"
#include "usb/usb_types_ch9.h"

static const char *TAG = "AOA_AUDIO";

/* AOA request numbers */
#define AOA_REQ_GET_PROTOCOL     51
#define AOA_REQ_SEND_STRING      52
#define AOA_REQ_START            53
#define AOA_REQ_SET_AUDIO_MODE   58

/* AOA string IDs */
#define AOA_STR_MANUFACTURER     0
#define AOA_STR_MODEL            1
#define AOA_STR_DESCRIPTION      2
#define AOA_STR_VERSION          3
#define AOA_STR_URI              4
#define AOA_STR_SERIAL           5

/* Google VID and AOA audio PIDs */
#define AOA_GOOGLE_VID           0x18D1
#define AOA_PID_AUDIO            0x2D02
#define AOA_PID_AUDIO_ADB        0x2D03
#define AOA_PID_ACC_AUDIO        0x2D04
#define AOA_PID_ACC_AUDIO_ADB    0x2D05

/* bmRequestType values used by AOA */
#define AOA_REQTYPE_IN_VENDOR    0xC0
#define AOA_REQTYPE_OUT_VENDOR   0x40

typedef struct {
    volatile bool done;
    usb_transfer_status_t status;
} aoa_ctrl_wait_t;

static void aoa_ctrl_cb(usb_transfer_t *transfer)
{
    aoa_ctrl_wait_t *wait = (aoa_ctrl_wait_t *)transfer->context;
    wait->status = transfer->status;
    wait->done = true;
}

/*
 * IMPORTANT:
 * This helper must be called from the same USB host client task context
 * that pumps usb_host_client_handle_events().
 */
static esp_err_t aoa_control_xfer(usb_host_client_handle_t client_hdl,
                                  usb_device_handle_t dev_hdl,
                                  uint8_t bmRequestType,
                                  uint8_t bRequest,
                                  uint16_t wValue,
                                  uint16_t wIndex,
                                  const void *out_data,
                                  uint16_t wLength,
                                  void *in_data)
{
    esp_err_t err;
    usb_transfer_t *xfer = NULL;
    aoa_ctrl_wait_t wait = {
        .done = false,
        .status = USB_TRANSFER_STATUS_NO_DEVICE
    };

    err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + wLength, 0, &xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_transfer_alloc failed");
        return err;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest      = bRequest;
    setup->wValue        = wValue;
    setup->wIndex        = wIndex;
    setup->wLength       = wLength;

    if ((bmRequestType & 0x80) == 0 && out_data != NULL && wLength > 0) {
        memcpy(xfer->data_buffer + USB_SETUP_PACKET_SIZE, out_data, wLength);
    }

    xfer->device_handle = dev_hdl;
    xfer->bEndpointAddress = 0;  // EP0 control
    xfer->callback = aoa_ctrl_cb;
    xfer->context = &wait;
    xfer->num_bytes = USB_SETUP_PACKET_SIZE + wLength;

    err = usb_host_transfer_submit_control(client_hdl, xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_transfer_submit_control failed: %s", esp_err_to_name(err));
        usb_host_transfer_free(xfer);
        return err;
    }

    while (!wait.done) {
        err = usb_host_client_handle_events(client_hdl, pdMS_TO_TICKS(10));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "usb_host_client_handle_events failed: %s", esp_err_to_name(err));
            usb_host_transfer_free(xfer);
            return err;
        }
    }

    if (wait.status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(TAG, "Control transfer failed, status=%d", wait.status);
        usb_host_transfer_free(xfer);
        return ESP_FAIL;
    }

    if ((bmRequestType & 0x80) != 0 && in_data != NULL && wLength > 0) {
        memcpy(in_data, xfer->data_buffer + USB_SETUP_PACKET_SIZE, wLength);
    }

    usb_host_transfer_free(xfer);
    return ESP_OK;
}

static esp_err_t aoa_get_protocol(usb_host_client_handle_t client_hdl,
                                  usb_device_handle_t dev_hdl,
                                  uint16_t *protocol_out)
{
    uint16_t proto = 0;
    esp_err_t err = aoa_control_xfer(client_hdl,
                                     dev_hdl,
                                     AOA_REQTYPE_IN_VENDOR,
                                     AOA_REQ_GET_PROTOCOL,
                                     0, 0,
                                     NULL,
                                     sizeof(proto),
                                     &proto);
    if (err != ESP_OK) {
        return err;
    }

    *protocol_out = proto;
    return ESP_OK;
}

static esp_err_t aoa_send_string(usb_host_client_handle_t client_hdl,
                                 usb_device_handle_t dev_hdl,
                                 uint16_t string_id,
                                 const char *str)
{
    size_t len = strlen(str) + 1; // include \0
    if (len > 256) {
        return ESP_ERR_INVALID_ARG;
    }

    return aoa_control_xfer(client_hdl,
                            dev_hdl,
                            AOA_REQTYPE_OUT_VENDOR,
                            AOA_REQ_SEND_STRING,
                            0,
                            string_id,
                            str,
                            (uint16_t)len,
                            NULL);
}

static esp_err_t aoa_set_audio_mode(usb_host_client_handle_t client_hdl,
                                    usb_device_handle_t dev_hdl)
{
    return aoa_control_xfer(client_hdl,
                            dev_hdl,
                            AOA_REQTYPE_OUT_VENDOR,
                            AOA_REQ_SET_AUDIO_MODE,
                            1,   // 1 = stereo, 16-bit PCM, 44.1 kHz
                            0,
                            NULL,
                            0,
                            NULL);
}

static esp_err_t aoa_start(usb_host_client_handle_t client_hdl,
                           usb_device_handle_t dev_hdl)
{
    return aoa_control_xfer(client_hdl,
                            dev_hdl,
                            AOA_REQTYPE_OUT_VENDOR,
                            AOA_REQ_START,
                            0,
                            0,
                            NULL,
                            0,
                            NULL);
}

bool aoa_is_audio_mode_device(uint16_t vid, uint16_t pid)
{
    if (vid != AOA_GOOGLE_VID) {
        return false;
    }

    return (pid == AOA_PID_AUDIO ||
            pid == AOA_PID_AUDIO_ADB ||
            pid == AOA_PID_ACC_AUDIO ||
            pid == AOA_PID_ACC_AUDIO_ADB);
}

bool aoa_is_android_google_vid(uint16_t vid)
{
    return (vid == AOA_GOOGLE_VID);
}

/*
 * Call this once after a phone first enumerates, BEFORE the UAC host driver
 * tries to claim interfaces.
 *
 * On success, the phone should disconnect and re-enumerate in AOA audio mode.
 */
esp_err_t aoa_try_enable_audio_mode(usb_host_client_handle_t client_hdl,
                                    usb_device_handle_t dev_hdl)
{
    uint16_t protocol = 0;
    esp_err_t err;

    err = aoa_get_protocol(client_hdl, dev_hdl, &protocol);
    if (err != ESP_OK || protocol == 0) {
        ESP_LOGW(TAG, "Device does not appear to support AOA");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "AOA protocol version: %u", protocol);

    // Keep these strings simple and non-empty.
    ESP_RETURN_ON_ERROR(aoa_send_string(client_hdl, dev_hdl, AOA_STR_MANUFACTURER, "UCF"), TAG, "manufacturer");
    ESP_RETURN_ON_ERROR(aoa_send_string(client_hdl, dev_hdl, AOA_STR_MODEL, "SeniorDesignSpeaker"), TAG, "model");
    ESP_RETURN_ON_ERROR(aoa_send_string(client_hdl, dev_hdl, AOA_STR_DESCRIPTION, "Android USB audio dock"), TAG, "description");
    ESP_RETURN_ON_ERROR(aoa_send_string(client_hdl, dev_hdl, AOA_STR_VERSION, "1.0"), TAG, "version");
    ESP_RETURN_ON_ERROR(aoa_send_string(client_hdl, dev_hdl, AOA_STR_URI, "https://example.com"), TAG, "uri");
    ESP_RETURN_ON_ERROR(aoa_send_string(client_hdl, dev_hdl, AOA_STR_SERIAL, "000001"), TAG, "serial");

    ESP_RETURN_ON_ERROR(aoa_set_audio_mode(client_hdl, dev_hdl), TAG, "set audio mode");
    ESP_RETURN_ON_ERROR(aoa_start(client_hdl, dev_hdl), TAG, "accessory start");

    ESP_LOGI(TAG, "AOA audio requested; wait for phone to re-enumerate");
    return ESP_OK;
}
