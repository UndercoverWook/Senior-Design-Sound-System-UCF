#include "ble_control.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config_functions.h"
#include "glb_params.h"
#include "help_functions.h"
#include "my_tasks.h"

extern "C" {
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

static const char *BLE_TAG = "BLE";
static const char *BLE_DEVICE_NAME = "ESP32_AutoEQ";

static uint8_t ble_own_addr_type = 0;
static uint16_t ble_tx_val_handle = 0;
static uint16_t ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool ble_notify_enabled = false;
static bool ble_histogram_enabled = false;

static const float kBandCentersHz[8] = {
    20.0f, 60.0f, 150.0f, 400.0f, 500.0f, 1000.0f, 2000.0f, 8000.0f,
};

static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(0xab, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t rx_char_uuid =
    BLE_UUID128_INIT(0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x78, 0x56,
                     0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xcd, 0xab);

static const ble_uuid128_t tx_char_uuid =
    BLE_UUID128_INIT(0x57, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x78, 0x56,
                     0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xcd, 0xab);

static void ble_start_advertising(void);
static void ble_send_text_notification(const char *text);
static void ble_start_calibration_from_app(void);
static void ble_handle_app_command(const char *cmd);

static int ble_gatt_access_cb(uint16_t conn_handle,
                              uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt,
                              void *arg);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);

static const struct ble_gatt_chr_def gatt_chars[] = {
    {
        .uuid = &rx_char_uuid.u,
        .access_cb = ble_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &tx_char_uuid.u,
        .access_cb = ble_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &ble_tx_val_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = gatt_chars,
    },
    {0},
};

static float ble_mag_to_db(float mag)
{
    float db = 20.0f * log10f(mag + 1e-9f);
    if (!isfinite(db) || db < 0.0f) {
        db = 0.0f;
    }
    if (db > 80.0f) {
        db = 80.0f;
    }
    return db;
}

static void ble_send_text_notification(const char *text)
{
    if (!ble_notify_enabled || ble_conn_handle == BLE_HS_CONN_HANDLE_NONE || text == NULL) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
    if (om == NULL) {
        ESP_LOGE(BLE_TAG, "Failed to allocate notify buffer");
        return;
    }

    int rc = ble_gatts_notify_custom(ble_conn_handle, ble_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(BLE_TAG, "Notify failed: %d", rc);
    }
}

void ble_send_app_message(const char *text)
{
    ble_send_text_notification(text);
}

static void ble_start_calibration_from_app(void)
{
    if (calibration_in_progress || wav_playback_active) {
        ble_send_text_notification("BUSY:CALIBRATION");
        ESP_LOGW(BLE_TAG, "Calibration request ignored because audio is already active");
        return;
    }

    calibration_in_progress = true;
    ble_histogram_enabled = true;
    ble_send_text_notification("ACK:AUTO_EQ_START");
    play_and_sample();
}

void ble_publish_fft_bins_from_complex(const float *fft_complex, float sample_rate)
{
    if (!ble_histogram_enabled || fft_complex == NULL || sample_rate <= 0.0f) {
        return;
    }

    char msg[160] = {0};
    int offset = snprintf(msg, sizeof(msg), "FFT:");

    for (size_t i = 0; i < 8; ++i) {
        int bin = (int)lroundf((kBandCentersHz[i] * FFT_SIZE) / sample_rate);
        if (bin < 0) {
            bin = 0;
        }
        if (bin >= NUM_BINS) {
            bin = NUM_BINS - 1;
        }

        const float re = fft_complex[bin * 2 + 0];
        const float im = fft_complex[bin * 2 + 1];
        const float mag = sqrtf((re * re) + (im * im));
        const float db = ble_mag_to_db(mag);

        offset += snprintf(
            msg + offset,
            sizeof(msg) - (size_t)offset,
            (i == 0) ? "%.1f" : ",%.1f",
            db);

        if (offset >= (int)sizeof(msg)) {
            break;
        }
    }

    ble_send_text_notification(msg);
}

static void ble_handle_app_command(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0') {
        return;
    }

    ESP_LOGI(BLE_TAG, "App command: %s", cmd);

    if (strcmp(cmd, "AUTO_EQ_START") == 0) {
        ble_start_calibration_from_app();
        return;
    }

    if (strcmp(cmd, "PLAY_WAV") == 0) {
        if (calibration_in_progress || wav_playback_active) {
            ESP_LOGW(BLE_TAG, "Ignoring PLAY_WAV while audio task is active");
            return;
        }

        wav_playback_active = true;
        BaseType_t rc = xTaskCreatePinnedToCore(
            vPlay_WAV_task,
            "WAV Playback",
            8192,
            NULL,
            configMAX_PRIORITIES - 1,
            NULL,
            CORE1);

        if (rc != pdPASS) {
            wav_playback_active = false;
            ESP_LOGE(BLE_TAG, "Failed to create WAV playback task");
            ble_send_text_notification("ERR:PLAY_WAV");
        }
        return;
    }

    if (strcmp(cmd, "HIST_ON") == 0) {
        ble_start_calibration_from_app();
        return;
    }

    if (strcmp(cmd, "HIST_OFF") == 0) {
        ble_histogram_enabled = false;
        return;
    }

    if (strcmp(cmd, "EQ_RESET") == 0) {
        ESP_LOGI(BLE_TAG, "EQ reset requested");
        return;
    }

    if (strncmp(cmd, "VOL:", 4) == 0) {
        ESP_LOGI(BLE_TAG, "Volume requested: %s", cmd + 4);
        return;
    }

    if (strncmp(cmd, "EQ", 2) == 0) {
        ESP_LOGI(BLE_TAG, "EQ band update: %s", cmd);
        return;
    }
}

static int ble_gatt_access_cb(uint16_t conn_handle,
                              uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt,
                              void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len <= 0) {
            return 0;
        }

        char buffer[256] = {0};
        if (len >= (int)sizeof(buffer)) {
            len = sizeof(buffer) - 1;
        }

        int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, len, NULL);
        if (rc != 0) {
            ESP_LOGE(BLE_TAG, "Failed to read BLE payload");
            return BLE_ATT_ERR_UNLIKELY;
        }

        buffer[len] = '\0';
        ble_handle_app_command(buffer);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ble_conn_handle = event->connect.conn_handle;
                ESP_LOGI(BLE_TAG, "Client connected, conn_handle=%u", ble_conn_handle);
            } else {
                ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                ble_notify_enabled = false;
                ble_histogram_enabled = false;
                ble_start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(BLE_TAG, "Client disconnected");
            ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_notify_enabled = false;
            ble_histogram_enabled = false;
            ble_start_advertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == ble_tx_val_handle) {
                ble_notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(
                    BLE_TAG,
                    "TX notifications %s",
                    ble_notify_enabled ? "enabled" : "disabled");

            }
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ble_start_advertising();
            return 0;

        default:
            return 0;
    }
}

static void ble_start_advertising(void)
{
    if (ble_gap_adv_active()) {
        return;
    }

    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(
        ble_own_addr_type,
        NULL,
        BLE_HS_FOREVER,
        &adv_params,
        ble_gap_event_cb,
        NULL);

    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "Advertising failed: %d", rc);
    } else {
        ESP_LOGI(BLE_TAG, "Advertising started");
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &ble_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    ble_start_advertising();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_control_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    ESP_ERROR_CHECK((rc == 0) ? ESP_OK : ESP_FAIL);

    rc = ble_gatts_add_svcs(gatt_svcs);
    ESP_ERROR_CHECK((rc == 0) ? ESP_OK : ESP_FAIL);

    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
}
