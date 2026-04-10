#include <stdio.h>
#include <string.h>
#include <math.h>
#include <algorithm>

#include "esp_dsp.h"

#include "my_lib/config_functions.h"
#include "my_lib/glb_params.h"
#include "my_lib/my_tasks.h"

extern "C" {
#include "freertos/semphr.h"

#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

static const char *TAG = "AUTOEQ_MAIN";
static const char *BLE_TAG = "BLE";
static const char *FFT_TAG = "FFT";

static uint8_t own_addr_type;
static uint16_t tx_chr_val_handle = 0;
static uint16_t current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool notify_enabled = false;

static char g_last_tx_value[128] = "READY";
static const uint8_t kRxCharTag = 1;
static const uint8_t kTxCharTag = 2;

static bool calibration_active = false;
static bool histogram_stream_enabled = false;
static bool capture_running = false;
static bool timer_enabled = false;
static bool pacer_callbacks_registered = false;
static bool fft_initialized = false;

static TaskHandle_t histogram_task_handle = NULL;
static TaskHandle_t fft_task_handle = NULL;
static SemaphoreHandle_t sample_sem = NULL;

static float wind[FFT_SIZE];
static float y_cf[FFT_SIZE * 2];
static float mag[NUM_BINS];

static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(0xab, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);
static const ble_uuid128_t rx_char_uuid =
    BLE_UUID128_INIT(0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x78, 0x56,
                     0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xcd, 0xab);
static const ble_uuid128_t tx_char_uuid =
    BLE_UUID128_INIT(0x57, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x78, 0x56,
                     0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xcd, 0xab);

static void start_advertising(void);
static void send_ble_text_notification(const char *text);
static void handle_app_command(const char *cmd);
static void start_histogram_stream(void);
static void stop_histogram_stream(void);
static void start_fft_capture(void);

static int gatt_access_cb(uint16_t conn_handle,
                          uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt,
                          void *arg);
static int gap_event_cb(struct ble_gap_event *event, void *arg);

static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer,
                                     const gptimer_alarm_event_data_t *edata,
                                     void *user_ctx)
{
    (void)timer;
    (void)edata;
    BaseType_t high_task_awoken = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    xSemaphoreGiveFromISR(sem, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}

void play_and_sample()
{
    sync_tasks = xEventGroupCreate();
    xTaskCreatePinnedToCore(vSample_task, "ADC Sampling", 8192, NULL, configMAX_PRIORITIES - 1, NULL, CORE0);
    xTaskCreatePinnedToCore(vPlay_WAV_task, "WAV Playback", 8192, NULL, configMAX_PRIORITIES - 1, NULL, CORE1);
}

static void set_last_tx_value(const char *text)
{
    if (text == NULL) {
        return;
    }

    strncpy(g_last_tx_value, text, sizeof(g_last_tx_value) - 1);
    g_last_tx_value[sizeof(g_last_tx_value) - 1] = '\0';
}

static void send_ble_text_notification(const char *text)
{
    if (text == NULL) {
        return;
    }

    set_last_tx_value(text);

    if (!notify_enabled || current_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
    if (om == NULL) {
        ESP_LOGE(BLE_TAG, "Failed to allocate notification buffer");
        return;
    }

    int rc = ble_gatts_notify_custom(current_conn_handle, tx_chr_val_handle, om);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "Notification failed: %d", rc);
    }
}

static void normalize_command(char *cmd)
{
    if (cmd == NULL) {
        return;
    }

    size_t len = strlen(cmd);
    while (len > 0) {
        char c = cmd[len - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            cmd[len - 1] = '\0';
            --len;
        } else {
            break;
        }
    }
}

static bool ensure_histogram_infra()
{
    if (spi_hdl == NULL) {
        configure_spi();
    }

    if (sync_timer == NULL) {
        initialize_pacer_timer(&sync_timer);
    }

    if (sample_sem == NULL) {
        sample_sem = xSemaphoreCreateBinary();
        if (sample_sem == NULL) {
            ESP_LOGE(FFT_TAG, "Failed to create sample semaphore");
            return false;
        }
    }

    if (!pacer_callbacks_registered) {
        gptimer_event_callbacks_t cbs = {
            .on_alarm = on_timer_alarm,
        };
        ESP_ERROR_CHECK(gptimer_register_event_callbacks(sync_timer, &cbs, sample_sem));
        pacer_callbacks_registered = true;
    }

    if (!fft_initialized) {
        esp_err_t ret = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(FFT_TAG, "FFT init failed: %s", esp_err_to_name(ret));
            return false;
        }
        fft_initialized = true;
    }

    return (spi_hdl != NULL && sync_timer != NULL);
}

static void build_fft_notification_from_bins(char *out, size_t out_len, float actual_fs)
{
    static const float centers[8] = {20.0f, 60.0f, 150.0f, 400.0f,
                                     500.0f, 1000.0f, 2000.0f, 8000.0f};
    static const int half_widths[8] = {1, 1, 1, 1, 1, 2, 2, 3};

    float selected[8] = {0};
    int peak_bins[8] = {0};

    for (int i = 0; i < 8; ++i) {
        int center_bin = (int)lroundf((centers[i] * FFT_SIZE) / actual_fs);
        if (center_bin < 1) center_bin = 1;
        if (center_bin >= NUM_BINS) center_bin = NUM_BINS - 1;

        int lo = center_bin - half_widths[i];
        int hi = center_bin + half_widths[i];
        if (lo < 1) lo = 1;
        if (hi >= NUM_BINS) hi = NUM_BINS - 1;

        float best = 0.0f;
        int best_bin = center_bin;
        for (int b = lo; b <= hi; ++b) {
            if (mag[b] > best) {
                best = mag[b];
                best_bin = b;
            }
        }

        float local_power = 0.0f;
        int count = 0;
        for (int b = std::max(1, best_bin - 1); b <= std::min(NUM_BINS - 1, best_bin + 1); ++b) {
            local_power += mag[b] * mag[b];
            ++count;
        }
        float local_rms = (count > 0) ? sqrtf(local_power / (float)count) : best;

        selected[i] = local_rms;
        peak_bins[i] = best_bin;
    }

    float frame_max = 0.0f;
    for (int i = 0; i < 8; ++i) {
        frame_max = std::max(frame_max, selected[i]);
    }
    if (frame_max < 1e-9f) {
        frame_max = 1e-9f;
    }

    int written = snprintf(out, out_len, "FFT:");
    if (written < 0 || (size_t)written >= out_len) {
        if (out_len > 0) out[0] = '\0';
        return;
    }
    size_t used = (size_t)written;

    for (int i = 0; i < 8; ++i) {
        float rel_db = 20.0f * log10f((selected[i] + 1e-9f) / frame_max);
        float norm = (rel_db + 24.0f) / 24.0f;
        int scaled = (int)lroundf(norm * 99.0f);
        if (scaled < 0) scaled = 0;
        if (scaled > 99) scaled = 99;

        const char *sep = (i == 0) ? "" : ",";
        written = snprintf(out + used, out_len - used, "%s%d", sep, scaled);
        if (written < 0 || (size_t)written >= (out_len - used)) {
            break;
        }
        used += (size_t)written;
    }
}

static void compute_fft_and_notify(uint16_t *samples, float actual_fs)
{
    dsps_wind_hann_f32(wind, FFT_SIZE);

    float mean = 0.0f;
    for (int i = 0; i < FFT_SIZE; ++i) {
        mean += (float)samples[i];
    }
    mean /= (float)FFT_SIZE;

    for (int i = 0; i < FFT_SIZE; ++i) {
        float normalized = ((float)samples[i] - mean) / 32768.0f;
        y_cf[i * 2 + 0] = normalized * wind[i];
        y_cf[i * 2 + 1] = 0.0f;
    }

    esp_err_t ret = dsps_fft2r_fc32(y_cf, FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(FFT_TAG, "FFT failed: %s", esp_err_to_name(ret));
        send_ble_text_notification("FFT_ERROR");
        return;
    }

    dsps_bit_rev_fc32(y_cf, FFT_SIZE);

    for (int i = 0; i < NUM_BINS; ++i) {
        float re = y_cf[i * 2 + 0];
        float im = y_cf[i * 2 + 1];
        mag[i] = (sqrtf(re * re + im * im) / (FFT_SIZE / 2)) * 2.0f;
    }

    char payload[96];
    memset(payload, 0, sizeof(payload));
    build_fft_notification_from_bins(payload, sizeof(payload), actual_fs);
    send_ble_text_notification(payload);
}

static void adc_fft_task(void *args)
{
    (void)args;
    capture_running = true;

    if (!ensure_histogram_infra()) {
        send_ble_text_notification("FFT_ERROR");
        capture_running = false;
        fft_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint16_t *samples = (uint16_t *)heap_caps_malloc(FFT_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (samples == NULL) {
        samples = (uint16_t *)calloc(FFT_SIZE, sizeof(uint16_t));
    }
    if (samples == NULL) {
        send_ble_text_notification("FFT_ERROR");
        capture_running = false;
        fft_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (xSemaphoreTake(sample_sem, 0) == pdTRUE) {
    }

    if (!timer_enabled) {
        ESP_ERROR_CHECK(gptimer_enable(sync_timer));
        timer_enabled = true;
    }

    uint64_t alarm_count = PACER_TIMER_HZ / SAMPLE_RATE;
    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = alarm_count,
        .reload_count = 0,
        .flags = {.auto_reload_on_alarm = true},
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(sync_timer, &alarm_cfg));
    ESP_ERROR_CHECK(gptimer_start(sync_timer));

    spi_transaction_t trans = {
        .flags = SPI_TRANS_USE_RXDATA,
        .length = TRANSACTION_LENGTH,
        .rxlength = TRANSACTION_LENGTH,
    };

    uint32_t t_start = esp_log_timestamp();
    bool capture_ok = true;

    for (int i = 0; i < FFT_SIZE; ++i) {
        if (xSemaphoreTake(sample_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            send_ble_text_notification("FFT_TIMEOUT");
            capture_ok = false;
            break;
        }

        memset(trans.rx_data, 0, sizeof(trans.rx_data));
        esp_err_t err = spi_device_polling_transmit(spi_hdl, &trans);
        if (err != ESP_OK) {
            send_ble_text_notification("FFT_SPI_ERROR");
            capture_ok = false;
            break;
        }

        uint32_t raw = ((uint32_t)trans.rx_data[0] << 16) |
                       ((uint32_t)trans.rx_data[1] << 8) |
                       (uint32_t)trans.rx_data[2];
        samples[i] = (raw >> 2) & 0xFFFF;
    }

    ESP_ERROR_CHECK(gptimer_stop(sync_timer));

    if (capture_ok) {
        uint32_t t_end = esp_log_timestamp();
        float actual_fs = (float)FFT_SIZE / (((t_end - t_start) / 1000.0f) + 1e-6f);
        ESP_LOGI(FFT_TAG, "Measured capture rate: %.1f Hz", actual_fs);
        compute_fft_and_notify(samples, actual_fs);
    }

    free(samples);
    capture_running = false;
    fft_task_handle = NULL;
    vTaskDelete(NULL);
}

static void histogram_stream_task(void *args)
{
    (void)args;
    ESP_LOGI(FFT_TAG, "Calibration histogram task started");

    while (histogram_stream_enabled && calibration_active) {
        if (notify_enabled && current_conn_handle != BLE_HS_CONN_HANDLE_NONE && !capture_running) {
            start_fft_capture();
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(FFT_TAG, "Calibration histogram task stopped");
    histogram_task_handle = NULL;
    vTaskDelete(NULL);
}

static void start_histogram_stream(void)
{
    if (histogram_stream_enabled) {
        return;
    }

    if (!ensure_histogram_infra()) {
        send_ble_text_notification("FFT_ERROR");
        return;
    }

    histogram_stream_enabled = true;
    BaseType_t rc = xTaskCreatePinnedToCore(
        histogram_stream_task,
        "LiveHistogram",
        8192,
        NULL,
        configMAX_PRIORITIES - 3,
        &histogram_task_handle,
        CORE0);

    if (rc != pdPASS) {
        ESP_LOGE(FFT_TAG, "Failed to create histogram task");
        histogram_stream_enabled = false;
        histogram_task_handle = NULL;
        send_ble_text_notification("FFT_ERROR");
    }
}

static void stop_histogram_stream(void)
{
    histogram_stream_enabled = false;
}

static void start_fft_capture(void)
{
    if (capture_running) {
        return;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        adc_fft_task,
        "ADC_FFT",
        8192,
        NULL,
        configMAX_PRIORITIES - 1,
        &fft_task_handle,
        CORE0);

    if (rc != pdPASS) {
        ESP_LOGE(FFT_TAG, "Failed to create ADC_FFT task");
        capture_running = false;
        fft_task_handle = NULL;
    }
}

static void handle_app_command(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0') {
        return;
    }

    ESP_LOGI(BLE_TAG, "Command received: %s", cmd);

    if (strcmp(cmd, "PLAY_WAV") == 0 || strcmp(cmd, "PLAY_AND_SAMPLE") == 0) {
        play_and_sample();
        send_ble_text_notification("ACK:PLAY_WAV");
        return;
    }

    if (strcmp(cmd, "HIST_ON") == 0 || strcmp(cmd, "AUTO_EQ") == 0) {
        calibration_active = true;
        start_histogram_stream();
        send_ble_text_notification("ACK:HIST_ON");
        return;
    }

    if (strcmp(cmd, "HIST_OFF") == 0) {
        calibration_active = false;
        stop_histogram_stream();
        send_ble_text_notification("ACK:HIST_OFF");
        return;
    }

    if (strcmp(cmd, "EQ_RESET") == 0) {
        ESP_LOGI(BLE_TAG, "EQ reset command received");
        send_ble_text_notification("ACK:EQ_RESET");
        return;
    }

    if (strncmp(cmd, "VOL:", 4) == 0) {
        ESP_LOGI(BLE_TAG, "Volume command payload: %s", cmd + 4);
        send_ble_text_notification("ACK:VOL");
        return;
    }

    if (strncmp(cmd, "EQ", 2) == 0) {
        ESP_LOGI(BLE_TAG, "EQ command payload: %s", cmd);
        send_ble_text_notification("ACK:EQ");
        return;
    }

    if (strncmp(cmd, "MODE:", 5) == 0) {
        ESP_LOGI(BLE_TAG, "Mode command payload: %s", cmd + 5);
        send_ble_text_notification("ACK:MODE");
        return;
    }

    ESP_LOGW(BLE_TAG, "Unknown command: %s", cmd);
    send_ble_text_notification("ERR:UNKNOWN_CMD");
}

static int gatt_access_cb(uint16_t conn_handle,
                          uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    (void)conn_handle;
    (void)attr_handle;

    const uint8_t char_tag = (arg == NULL) ? 0 : *(const uint8_t *)arg;

    if (char_tag == kRxCharTag) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }

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
            return BLE_ATT_ERR_UNLIKELY;
        }

        buffer[len] = '\0';
        normalize_command(buffer);
        handle_app_command(buffer);
        return 0;
    }

    if (char_tag == kTxCharTag) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            int rc = os_mbuf_append(ctxt->om, g_last_tx_value, strlen(g_last_tx_value));
            return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def gatt_chars[] = {
    {
        .uuid = &rx_char_uuid.u,
        .access_cb = gatt_access_cb,
        .arg = (void *)&kRxCharTag,
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle = NULL,
    },
    {
        .uuid = &tx_char_uuid.u,
        .access_cb = gatt_access_cb,
        .arg = (void *)&kTxCharTag,
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &tx_chr_val_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .includes = NULL,
        .characteristics = gatt_chars,
    },
    {0},
};

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                current_conn_handle = event->connect.conn_handle;
                notify_enabled = false;
                ESP_LOGI(BLE_TAG, "Client connected, conn_handle=%u", current_conn_handle);
            } else {
                ESP_LOGW(BLE_TAG, "Connect failed, status=%d", event->connect.status);
                current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                notify_enabled = false;
                calibration_active = false;
                stop_histogram_stream();
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(BLE_TAG, "Client disconnected, reason=%d", event->disconnect.reason);
            current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            notify_enabled = false;
            calibration_active = false;
            stop_histogram_stream();
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == tx_chr_val_handle) {
                notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(BLE_TAG, "TX notifications %s", notify_enabled ? "enabled" : "disabled");
                if (notify_enabled) {
                    send_ble_text_notification("READY");
                } else {
                    calibration_active = false;
                    stop_histogram_stream();
                }
            }
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(BLE_TAG, "Advertising complete, reason=%d", event->adv_complete.reason);
            start_advertising();
            return 0;

        default:
            return 0;
    }
}

static void start_advertising(void)
{
    if (current_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    if (ble_gap_adv_active()) {
        return;
    }

    struct ble_hs_adv_fields adv_fields;
    struct ble_hs_adv_fields rsp_fields;
    struct ble_gap_adv_params adv_params;

    memset(&adv_fields, 0, sizeof(adv_fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    memset(&adv_params, 0, sizeof(adv_params));

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    const char *name = "ESP32_AutoEQ";
    rsp_fields.name = (uint8_t *)name;
    rsp_fields.name_len = strlen(name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x0030;
    adv_params.itvl_max = 0x0060;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc == BLE_HS_EALREADY) {
        ESP_LOGW(BLE_TAG, "Advertising already active");
        return;
    }

    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "Advertising failed: %d", rc);
    } else {
        ESP_LOGI(BLE_TAG, "Advertising started");
    }
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_hs_id_infer_auto failed: %d", rc);
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

extern "C" void app_main(void)
{
    configure_psram();
    configure_spiffs();
    reconfigure_wdt();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(BLE_TAG, "Initializing NimBLE...");
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(BLE_TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }

    ble_svc_gap_device_name_set("ESP32_AutoEQ");
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "System ready. Waiting for BLE commands...");
}
