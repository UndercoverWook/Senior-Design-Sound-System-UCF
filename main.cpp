#include "driver/uart.h"
#include "hal/uart_types.h"
#include "soc/clk_tree_defs.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "driver/i2s_common.h"
#include "driver/i2s_types.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wave.h"
#include "wave_common.h"
#include "wave_reader.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

static const char *TAG = "BLE_WAV_PLAYER";
static const char *WAV_FILE = "WAV Test";

// -------------------------------------------------------------------------------------------------
// Audio globals
// -------------------------------------------------------------------------------------------------
static i2s_chan_handle_t mcu_rx = NULL;
static i2s_chan_handle_t mcu_tx = NULL;
static wave_reader_handle_t wav_hdl = NULL;

static TaskHandle_t playback_task_handle = NULL;
static bool isPlaying = false;

// -------------------------------------------------------------------------------------------------
// BLE globals
// -------------------------------------------------------------------------------------------------
static uint8_t own_addr_type;
static uint16_t tx_chr_val_handle = 0;
static uint16_t current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool notify_enabled = false;

// -------------------------------------------------------------------------------------------------
// Pins / audio settings
// -------------------------------------------------------------------------------------------------
static const gpio_num_t I2S_DIN_LINE  = GPIO_NUM_2;
static const gpio_num_t I2S_LRCLK_PIN = GPIO_NUM_4;
static const gpio_num_t I2S_BCLK_PIN  = GPIO_NUM_5;
static const gpio_num_t I2S_DOUT_LINE = GPIO_NUM_6;

#define SAMPLE_RATE      44100
#define BUFFER_FRAMES    1024
#define BYTES_PER_SAMPLE 2
#define CHANNELS         2
#define FRAME_SIZE_BYTES (BYTES_PER_SAMPLE * CHANNELS)
#define BUFFER_BYTES     (BUFFER_FRAMES * FRAME_SIZE_BYTES)

// -------------------------------------------------------------------------------------------------
// BLE UUIDs
// -------------------------------------------------------------------------------------------------
static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(0xab, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t rx_char_uuid =
    BLE_UUID128_INIT(0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x78, 0x56,
                     0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xcd, 0xab);

static const ble_uuid128_t tx_char_uuid =
    BLE_UUID128_INIT(0x57, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x78, 0x56,
                     0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xcd, 0xab);

// Forward declarations
static void start_advertising(void);
static void start_wav_playback(void);
static void send_fft_notification(const char *text);
static void handle_app_command(const char *cmd);

static int gatt_access_cb(uint16_t conn_handle,
                          uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt,
                          void *arg);

static int gap_event_cb(struct ble_gap_event *event, void *arg);

// -------------------------------------------------------------------------------------------------
// I2S init
// -------------------------------------------------------------------------------------------------
static void i2s_bt_to_mcu_to_dac_init()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 512;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &mcu_tx, &mcu_rx));

    i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    clk_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_config,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_LRCLK_PIN,
            .dout = I2S_DOUT_LINE,
            .din = I2S_DIN_LINE,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mcu_rx, &std_cfg));
}

// -------------------------------------------------------------------------------------------------
// SPIFFS init
// -------------------------------------------------------------------------------------------------
static void configure_spiffs()
{
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false
    };

    esp_err_t err = esp_vfs_spiffs_register(&spiffs_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to mount SPIFFS, ERROR: %s", esp_err_to_name(err));
        return;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info("storage", &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    }
}

// -------------------------------------------------------------------------------------------------
// WAV helpers
// -------------------------------------------------------------------------------------------------
static void print_wav(wave_header_t *wav_head)
{
    ESP_LOGI(WAV_FILE, "Bytes per sample: %u", wav_head->bytes_per_sample);
    ESP_LOGI(WAV_FILE, "# of channels: %u", wav_head->n_channels);
    ESP_LOGI(WAV_FILE, "Sample Rate: %u", wav_head->sample_rate);
    ESP_LOGI(WAV_FILE, "Samples per Channel: %u", wav_head->samples_per_channel);
}

static void playback_task(void *args)
{
    wave_header_t wav_head;
    wav_hdl = wave_reader_open("/storage/1khz_sine_44_1.wav");

    if (wav_hdl == NULL) {
        ESP_LOGE(WAV_FILE, "Unable to open WAV file for reading");
        isPlaying = false;
        playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (wave_read_header(wav_hdl, &wav_head) != 0) {
        ESP_LOGE(WAV_FILE, "Unable to read WAV header");
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        isPlaying = false;
        playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    print_wav(&wav_head);

    uint8_t *buff = (uint8_t *)calloc(1, BUFFER_BYTES);
    if (buff == NULL) {
        ESP_LOGE(TAG, "Failed to allocate playback buffer");
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        isPlaying = false;
        playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(i2s_channel_enable(mcu_tx));

    size_t pos = 0;

    while (1) {
        size_t bytes_read = wave_read_raw_data(wav_hdl, buff, pos, BUFFER_BYTES);

        if (bytes_read == 0) {
            ESP_LOGI(TAG, "WAV playback finished");
            break;
        }

        pos += bytes_read;

        size_t bytes_remaining = bytes_read;
        uint8_t *p = buff;

        while (bytes_remaining > 0) {
            size_t wrote = 0;
            esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_remaining, &wrote, portMAX_DELAY);

            if (r != ESP_OK) {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(r));
                bytes_remaining = 0;
                break;
            }

            if (wrote == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            bytes_remaining -= wrote;
            p += wrote;
        }
    }

    i2s_channel_disable(mcu_tx);
    wave_reader_close(wav_hdl);
    wav_hdl = NULL;
    free(buff);

    isPlaying = false;
    playback_task_handle = NULL;
    vTaskDelete(NULL);
}

static void start_wav_playback(void)
{
    if (isPlaying) {
        ESP_LOGW(TAG, "Playback already running");
        return;
    }

    isPlaying = true;

    BaseType_t rc = xTaskCreatePinnedToCore(
        playback_task,
        "Playback",
        8192,
        NULL,
        configMAX_PRIORITIES - 1,
        &playback_task_handle,
        1
    );

    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        isPlaying = false;
        playback_task_handle = NULL;
        return;
    }

    send_fft_notification("FFT:10,18,30,24,16,12,20,28,22,14");
}

// -------------------------------------------------------------------------------------------------
// BLE notifications
// -------------------------------------------------------------------------------------------------
static void send_fft_notification(const char *text)
{
    if (!notify_enabled) {
        ESP_LOGW(TAG, "Notify not enabled yet");
        return;
    }

    if (current_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "No active BLE connection");
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate notify mbuf");
        return;
    }

    int rc = ble_gatts_notify_custom(current_conn_handle, tx_chr_val_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Notify failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Notify sent: %s", text);
    }
}

// -------------------------------------------------------------------------------------------------
// Command parser
// -------------------------------------------------------------------------------------------------
static void handle_app_command(const char *cmd)
{
    if (cmd == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Command received: %s", cmd);

    if (strcmp(cmd, "PLAY_WAV") == 0) {
        start_wav_playback();
    } else if (strcmp(cmd, "EQ_RESET") == 0) {
        ESP_LOGI(TAG, "EQ reset command received");
    } else if (strncmp(cmd, "VOL:", 4) == 0) {
        ESP_LOGI(TAG, "Volume command = %s", cmd + 4);
    } else if (strncmp(cmd, "MODE:", 5) == 0) {
        ESP_LOGI(TAG, "Mode command = %s", cmd + 5);
    } else if (strncmp(cmd, "EQ", 2) == 0) {
        ESP_LOGI(TAG, "EQ command = %s", cmd);
    } else if (strcmp(cmd, "FFT_TEST") == 0) {
        send_fft_notification("FFT:12,15,21,30,27,22,18,14,10,8");
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
    }
}

// -------------------------------------------------------------------------------------------------
// GATT access callback
// -------------------------------------------------------------------------------------------------
static int gatt_access_cb(uint16_t conn_handle,
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

        char buffer[256];
        memset(buffer, 0, sizeof(buffer));

        if (len >= (int)sizeof(buffer)) {
            len = sizeof(buffer) - 1;
        }

        int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, len, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to extract BLE write payload");
            return BLE_ATT_ERR_UNLIKELY;
        }

        buffer[len] = '\0';
        handle_app_command(buffer);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

// -------------------------------------------------------------------------------------------------
// GATT table - separate static arrays to avoid malformed temporary initializers
// -------------------------------------------------------------------------------------------------
static const struct ble_gatt_chr_def gatt_chars[] = {
    {
        .uuid = &rx_char_uuid.u,
        .access_cb = gatt_access_cb,
        .arg = NULL,
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle = NULL,
    },
    {
        .uuid = &tx_char_uuid.u,
        .access_cb = gatt_access_cb,
        .arg = NULL,
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &tx_chr_val_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .includes = NULL,
        .characteristics = gatt_chars,
    },
    { 0 }
};

// -------------------------------------------------------------------------------------------------
// GAP callback
// -------------------------------------------------------------------------------------------------
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                current_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "Client connected, conn_handle=%u", current_conn_handle);
            } else {
                current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                notify_enabled = false;
                ESP_LOGI(TAG, "Connect failed; restarting advertising");
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Client disconnected");
            current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            notify_enabled = false;
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == tx_chr_val_handle) {
                notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "TX notifications %s", notify_enabled ? "enabled" : "disabled");

                if (notify_enabled) {
                    send_fft_notification("FFT:8,12,16,20,24,20,16,12,8,6");
                }
            }
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG, "Advertising complete; restarting");
            start_advertising();
            return 0;

        default:
            return 0;
    }
}

// -------------------------------------------------------------------------------------------------
// Advertising
// -------------------------------------------------------------------------------------------------
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

    // Do NOT add the 128-bit service UUID here.
    // It makes the advertising packet too large.

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
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
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// -------------------------------------------------------------------------------------------------
// Main
// -------------------------------------------------------------------------------------------------
extern "C" void app_main(void)
{
    uart_driver_delete(UART_NUM_0);

    i2s_bt_to_mcu_to_dac_init();
    configure_spiffs();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing NimBLE...");

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }

    ble_svc_gap_device_name_set("ESP32_AutoEQ");
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "System ready. Waiting for BLE commands...");
}