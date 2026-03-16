#include <stdio.h>
#include <string.h>
#include <assert.h>

extern "C" {
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

static const char *TAG = "BLE_LOGGER";

// UUIDs for your custom BLE service/characteristic
static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(0xab, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t char_uuid =
    BLE_UUID128_INIT(0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x78, 0x56,
                     0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xcd, 0xab);

static uint8_t own_addr_type;

// Forward declarations
static void start_advertising(void);

static int gatt_access_cb(uint16_t conn_handle,
                          uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        int len = OS_MBUF_PKTLEN(ctxt->om);

        if (len > 0) {
            char buffer[256] = {0};

            if (len >= (int)sizeof(buffer)) {
                len = sizeof(buffer) - 1;
            }

            int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, len, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to read incoming BLE data");
                return BLE_ATT_ERR_UNLIKELY;
            }

            buffer[len] = '\0';

            printf("\n----- BLE Command Received -----\n");
            printf("Text: %s\n", buffer);

            printf("Bytes: ");
            for (int i = 0; i < len; i++) {
                printf("0x%02X ", (uint8_t)buffer[i]);
            }
            printf("\n");

            // Simple command parsing examples
            if (strncmp(buffer, "VOL:", 4) == 0) {
                printf("Parsed Volume = %s\n", buffer + 4);
            } else if (strncmp(buffer, "MODE:", 5) == 0) {
                printf("Parsed Mode = %s\n", buffer + 5);
            } else if (strncmp(buffer, "EQ:", 3) == 0) {
                printf("Parsed EQ = %s\n", buffer + 3);
            }

            printf("-------------------------------\n\n");
        }
    }

    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &char_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0}
        },
    },
    {0}
};

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Client connected");
            } else {
                ESP_LOGI(TAG, "Connection failed, restarting advertising");
                start_advertising();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Client disconnected");
            start_advertising();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG, "Advertising complete, restarting");
            start_advertising();
            break;

        default:
            break;
    }

    return 0;
}

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = "ESP32_AutoEQ";
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising started");
    }
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    start_advertising();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing NimBLE...");

    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);

    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);

    ble_svc_gap_device_name_set("ESP32_AutoEQ");
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);
}