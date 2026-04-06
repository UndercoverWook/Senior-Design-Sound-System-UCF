#include "esp_dsp.h"
#include "driver/uart.h"
#include "hal/uart_types.h"
#include "soc/clk_tree_defs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_log_timestamp.h"
#include "esp_psram.h"
#include "esp_rom_sys.h"
#include "esp_spiffs.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#include "hal/i2s_types.h"
#include "hal/spi_types.h"
#include "soc/gpio_num.h"

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

static const char *TAG = "AUTOEQ_COMBINED";
static const char *STORAGE_TAG = "FileSystem";
static const char *SPI_TAG = "SPI";
static const char *I2S_TAG = "I2S";
static const char *FFT_TAG = "FFT";
static const char *WAV_TAG = "WAV";
static const char *BLE_TAG = "BLE";

#define SAMPLE_RATE        44100
#define BUFFER_FRAMES      1024
#define BYTES_PER_SAMPLE   2
#define CHANNELS           1
#define FRAME_SIZE_BYTES   (BYTES_PER_SAMPLE * CHANNELS)
#define BUFFER_BYTES       (BUFFER_FRAMES * FRAME_SIZE_BYTES)
#define PACER_TIMER_HZ     10000000ULL
#define FFT_SIZE           2048
#define NUM_BINS           (FFT_SIZE / 2)
#define DATA_LENGTH        24
#define STACK_DEPTH        8192
#define CORE0              0
#define CORE1              1

#define TEST_WAV_SECONDS   5
#define I2S_TX_CHANNELS    2

static const gpio_num_t I2S_RX_LINE    = GPIO_NUM_2;
static const gpio_num_t ADC_CS_PIN     = GPIO_NUM_10;
static const gpio_num_t I2S_LRCLK_PIN  = GPIO_NUM_11;
static const gpio_num_t ADC_SCLK_PIN   = GPIO_NUM_12;
static const gpio_num_t ADC_MISO_PIN   = GPIO_NUM_13;
static const gpio_num_t I2S_TX_LINE    = GPIO_NUM_14;
static const gpio_num_t I2S_BIT_CLK    = GPIO_NUM_21;

static gptimer_handle_t sync_timer = NULL;
static spi_device_handle_t spi_hdl = NULL;
static i2s_chan_handle_t mcu_rx = NULL;
static i2s_chan_handle_t mcu_tx = NULL;
static wave_reader_handle_t wav_hdl = NULL;

static TaskHandle_t playback_task_handle = NULL;
static TaskHandle_t sample_task_handle = NULL;
static TaskHandle_t histogram_task_handle = NULL;

static bool isPlaying = false;
static bool capture_running = false;
static bool histogram_stream_enabled = false;
static bool calibration_active = false;

static float wind[FFT_SIZE];
static float y_cf[FFT_SIZE * 2];
static float mag[NUM_BINS];

static uint8_t own_addr_type;
static uint16_t tx_chr_val_handle = 0;
static uint16_t current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool notify_enabled = false;
static bool timer_callbacks_registered = false;
static bool timer_enabled = false;
static SemaphoreHandle_t sample_sem = NULL;

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
static void start_wav_playback(void);
static void start_fft_capture(void);
static void start_histogram_stream(void);
static void stop_histogram_stream(void);
static void send_ble_text_notification(const char *text);
static void handle_app_command(const char *cmd);

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

static void send_ble_text_notification(const char *text)
{
    if (!notify_enabled || current_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
    if (om == NULL) {
        ESP_LOGE(BLE_TAG, "Failed to allocate notify mbuf");
        return;
    }

    int rc = ble_gatts_notify_custom(current_conn_handle, tx_chr_val_handle, om);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "Notify failed: %d", rc);
    }
}

static void configure_spiffs()
{
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&spiffs_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(STORAGE_TAG, "Unable to mount SPIFFS: %s", esp_err_to_name(err));
        return;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info("storage", &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(STORAGE_TAG, "SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    }
}

static void configure_psram()
{
#if CONFIG_SPIRAM
    esp_err_t psram_err = esp_psram_init();
    if (psram_err != ESP_OK) {
        ESP_LOGW(STORAGE_TAG, "PSRAM init failed: %s", esp_err_to_name(psram_err));
    }
#endif
}

static void configure_spi()
{
    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = -1,
        .miso_io_num = ADC_MISO_PIN,
        .sclk_io_num = ADC_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE,
    };

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &spi_bus_cfg, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(SPI_TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return;
    }

    spi_device_interface_config_t ads8320_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,
        .clock_source = SPI_CLK_SRC_DEFAULT,
        .clock_speed_hz = 2000000,
        .input_delay_ns = 0,
        .spics_io_num = ADC_CS_PIN,
        .queue_size = 1,
    };

    err = spi_bus_add_device(SPI2_HOST, &ads8320_cfg, &spi_hdl);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(SPI_TAG, "SPI device already added, continuing");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(SPI_TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    }
}

static void configure_i2s()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 512;

    esp_err_t err = i2s_new_channel(&chan_cfg, &mcu_tx, &mcu_rx);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(I2S_TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return;
    }

    i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    clk_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_config,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = I2S_BIT_CLK,
            .ws = I2S_LRCLK_PIN,
            .dout = I2S_TX_LINE,
            .din = I2S_RX_LINE,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(mcu_tx, &std_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(I2S_TAG, "i2s_channel_init_std_mode TX failed: %s", esp_err_to_name(err));
    }

    err = i2s_channel_init_std_mode(mcu_rx, &std_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(I2S_TAG, "i2s_channel_init_std_mode RX failed: %s", esp_err_to_name(err));
    }
}

static void initialize_pacer_timer(gptimer_handle_t *t)
{
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = PACER_TIMER_HZ,
    };

    esp_err_t err = gptimer_new_timer(&cfg, t);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
    }
}

static void print_wav(wave_header_t *wav_head)
{
    ESP_LOGI(WAV_TAG, "Bytes/sample: %u", wav_head->bytes_per_sample);
    ESP_LOGI(WAV_TAG, "Channels: %u", wav_head->n_channels);
    ESP_LOGI(WAV_TAG, "Sample Rate: %u", wav_head->sample_rate);
    ESP_LOGI(WAV_TAG, "Samples/Channel: %u", wav_head->samples_per_channel);
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
    for (float v : selected) frame_max = std::max(frame_max, v);
    if (frame_max < 1e-9f) frame_max = 1e-9f;

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

        float peak_hz = ((float)peak_bins[i] * actual_fs) / FFT_SIZE;
        float abs_db = 20.0f * log10f(selected[i] + 1e-9f);
        ESP_LOGI(FFT_TAG,
                 "Bar %.0f Hz: peakbin=%d (%.1f Hz), rms=%.4f (%.1f dB), rel=%.1f dB => %d",
                 centers[i], peak_bins[i], peak_hz, selected[i], abs_db, rel_db, scaled);

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
    for (int i = 0; i < FFT_SIZE; i++) {
        mean += (float)samples[i];
    }
    mean /= (float)FFT_SIZE;

    for (int i = 0; i < FFT_SIZE; i++) {
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

    for (int i = 0; i < NUM_BINS; i++) {
        float re = y_cf[i * 2 + 0];
        float im = y_cf[i * 2 + 1];
        mag[i] = (sqrtf(re * re + im * im) / (FFT_SIZE / 2)) * 2.0f;
    }

    int k1 = (int)lroundf((1000.0f * FFT_SIZE) / actual_fs);
    for (int i = k1 - 6; i <= k1 + 6; ++i) {
        if (i < 1 || i >= NUM_BINS) continue;
        float freq_hz = (float)i * actual_fs / FFT_SIZE;
        float mag_db = 20.0f * log10f(mag[i] + 1e-9f);
        ESP_LOGI(FFT_TAG, "Near-1k bin %d -> %8.1f Hz | %.2f dB", i, freq_hz, mag_db);
    }

    char payload[96];
    memset(payload, 0, sizeof(payload));
    build_fft_notification_from_bins(payload, sizeof(payload), actual_fs);
    send_ble_text_notification(payload);
}

static void playback_task(void *args)
{
    (void)args;

    wave_header_t wav_head;
    wav_hdl = wave_reader_open("/storage/1khz_sine_44_1.wav");

    if (wav_hdl == NULL) {
        ESP_LOGE(WAV_TAG, "Unable to open WAV file");
        isPlaying = false;
        playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (wave_read_header(wav_hdl, &wav_head) != 0) {
        ESP_LOGE(WAV_TAG, "Unable to read WAV header");
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        isPlaying = false;
        playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    print_wav(&wav_head);

    if (wav_head.bytes_per_sample != 2) {
        ESP_LOGE(WAV_TAG, "Only 16-bit PCM WAV is supported");
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        isPlaying = false;
        playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (wav_head.n_channels != 1 && wav_head.n_channels != 2) {
        ESP_LOGE(WAV_TAG, "Unsupported channel count: %u", wav_head.n_channels);
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        isPlaying = false;
        playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (wav_head.sample_rate != SAMPLE_RATE) {
        ESP_LOGW(WAV_TAG,
                 "WAV sample rate is %u Hz but I2S is configured for %u Hz",
                 wav_head.sample_rate, SAMPLE_RATE);
    }

    const size_t in_frame_bytes =
        (size_t)wav_head.bytes_per_sample * (size_t)wav_head.n_channels;

    const size_t max_frames_to_play =
        std::min<size_t>((size_t)wav_head.samples_per_channel,
                         (size_t)wav_head.sample_rate * TEST_WAV_SECONDS);

    const size_t input_chunk_bytes = BUFFER_FRAMES * in_frame_bytes;

    uint8_t *in_buf = (uint8_t *)calloc(1, input_chunk_bytes);
    int16_t *stereo_buf = (int16_t *)calloc(BUFFER_FRAMES * I2S_TX_CHANNELS, sizeof(int16_t));

    if (in_buf == NULL || stereo_buf == NULL) {
        ESP_LOGE(WAV_TAG, "Failed to allocate playback buffers");
        free(in_buf);
        free(stereo_buf);
        wave_reader_close(wav_hdl);
        wav_hdl = NULL;
        isPlaying = false;
        playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(i2s_channel_enable(mcu_tx));

    size_t pos = 0;
    size_t frames_sent = 0;

    while (frames_sent < max_frames_to_play) {
        size_t frames_remaining = max_frames_to_play - frames_sent;
        size_t frames_to_read = std::min<size_t>(BUFFER_FRAMES, frames_remaining);
        size_t bytes_to_read = frames_to_read * in_frame_bytes;

        size_t bytes_read = wave_read_raw_data(wav_hdl, in_buf, pos, bytes_to_read);
        if (bytes_read == 0) {
            ESP_LOGI(WAV_TAG, "WAV playback finished at EOF");
            break;
        }

        pos += bytes_read;

        bytes_read -= (bytes_read % in_frame_bytes);
        if (bytes_read == 0) {
            break;
        }

        size_t frames_read = bytes_read / in_frame_bytes;

        const uint8_t *out_ptr = NULL;
        size_t out_bytes = 0;

        if (wav_head.n_channels == 1) {
            const int16_t *mono = (const int16_t *)in_buf;
            for (size_t i = 0; i < frames_read; ++i) {
                int16_t s = mono[i];
                stereo_buf[2 * i + 0] = s;
                stereo_buf[2 * i + 1] = s;
            }
            out_ptr = (const uint8_t *)stereo_buf;
            out_bytes = frames_read * I2S_TX_CHANNELS * sizeof(int16_t);
        } else {
            out_ptr = in_buf;
            out_bytes = frames_read * I2S_TX_CHANNELS * sizeof(int16_t);
        }

        size_t bytes_remaining = out_bytes;
        const uint8_t *p = out_ptr;

        while (bytes_remaining > 0) {
            size_t wrote = 0;
            esp_err_t r = i2s_channel_write(mcu_tx, p, bytes_remaining, &wrote, portMAX_DELAY);
            if (r != ESP_OK) {
                ESP_LOGE(WAV_TAG, "I2S write failed: %s", esp_err_to_name(r));
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

        frames_sent += frames_read;
    }

    ESP_LOGI(WAV_TAG, "Playback stopped after %u frames", (unsigned)frames_sent);

    i2s_channel_disable(mcu_tx);
    wave_reader_close(wav_hdl);
    wav_hdl = NULL;
    free(in_buf);
    free(stereo_buf);

    isPlaying = false;
    playback_task_handle = NULL;
    vTaskDelete(NULL);
}

static void adc_fft_task(void *args)
{
    (void)args;
    capture_running = true;

    uint16_t *samples = (uint16_t *)heap_caps_malloc(FFT_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (samples == NULL) {
        samples = (uint16_t *)calloc(FFT_SIZE, sizeof(uint16_t));
    }
    if (samples == NULL) {
        send_ble_text_notification("FFT_ERROR");
        capture_running = false;
        sample_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (sample_sem == NULL) {
        ESP_LOGE(TAG, "Sample semaphore not initialized");
        send_ble_text_notification("FFT_ERROR");
        free(samples);
        capture_running = false;
        sample_task_handle = NULL;
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
        .length = DATA_LENGTH,
        .rxlength = DATA_LENGTH,
    };

    uint32_t t_start = esp_log_timestamp();

    for (int i = 0; i < FFT_SIZE; ++i) {
        if (xSemaphoreTake(sample_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            send_ble_text_notification("FFT_TIMEOUT");
            break;
        }

        memset(trans.rx_data, 0, sizeof(trans.rx_data));
        esp_err_t err = spi_device_polling_transmit(spi_hdl, &trans);
        if (err != ESP_OK) {
            send_ble_text_notification("FFT_SPI_ERROR");
            break;
        }

        uint32_t raw = ((uint32_t)trans.rx_data[0] << 16) |
                       ((uint32_t)trans.rx_data[1] << 8) |
                       (uint32_t)trans.rx_data[2];
        samples[i] = (raw >> 2) & 0xFFFF;
    }

    ESP_ERROR_CHECK(gptimer_stop(sync_timer));

    uint32_t t_end = esp_log_timestamp();
    float actual_fs = (float)FFT_SIZE / (((t_end - t_start) / 1000.0f) + 1e-6f);
    ESP_LOGI(FFT_TAG, "Measured capture rate: %.1f Hz", actual_fs);
    compute_fft_and_notify(samples, actual_fs);

    free(samples);
    capture_running = false;
    sample_task_handle = NULL;
    vTaskDelete(NULL);
}

static void histogram_stream_task(void *args)
{
    (void)args;
    ESP_LOGI(TAG, "Calibration histogram task started");

    while (histogram_stream_enabled && calibration_active) {
        if (notify_enabled && current_conn_handle != BLE_HS_CONN_HANDLE_NONE && !capture_running) {
            start_fft_capture();
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    ESP_LOGI(TAG, "Calibration histogram task stopped");
    histogram_task_handle = NULL;
    vTaskDelete(NULL);
}

static void start_histogram_stream()
{
    if (histogram_stream_enabled) return;

    histogram_stream_enabled = true;
    BaseType_t rc = xTaskCreatePinnedToCore(
        histogram_stream_task,
        "LiveHistogram",
        STACK_DEPTH,
        NULL,
        configMAX_PRIORITIES - 3,
        &histogram_task_handle,
        CORE0);

    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create live histogram task");
        histogram_stream_enabled = false;
        histogram_task_handle = NULL;
    }
}

static void stop_histogram_stream()
{
    histogram_stream_enabled = false;
}

static void start_wav_playback()
{
    if (isPlaying) {
        ESP_LOGW(WAV_TAG, "Playback already running");
        return;
    }

    isPlaying = true;
    BaseType_t rc = xTaskCreatePinnedToCore(
        playback_task,
        "Playback",
        STACK_DEPTH,
        NULL,
        configMAX_PRIORITIES - 2,
        &playback_task_handle,
        CORE1);

    if (rc != pdPASS) {
        ESP_LOGE(WAV_TAG, "Failed to create playback task");
        isPlaying = false;
        playback_task_handle = NULL;
    }
}

static void start_fft_capture()
{
    if (capture_running) return;

    BaseType_t rc = xTaskCreatePinnedToCore(
        adc_fft_task,
        "ADC_FFT",
        STACK_DEPTH,
        NULL,
        configMAX_PRIORITIES - 1,
        &sample_task_handle,
        CORE0);

    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ADC_FFT task");
        capture_running = false;
        sample_task_handle = NULL;
    }
}

static void handle_app_command(const char *cmd)
{
    if (cmd == NULL) return;

    ESP_LOGI(TAG, "Command received: %s", cmd);

    if (strcmp(cmd, "PLAY_WAV") == 0) {
        start_wav_playback();
    } else if (strcmp(cmd, "FFT_TEST") == 0) {
        start_fft_capture();
    } else if (strcmp(cmd, "AUTO_EQ") == 0 || strcmp(cmd, "HIST_ON") == 0) {
        calibration_active = true;
        start_histogram_stream();
    } else if (strcmp(cmd, "HIST_OFF") == 0) {
        calibration_active = false;
        stop_histogram_stream();
    } else if (strcmp(cmd, "EQ_RESET") == 0) {
        ESP_LOGI(TAG, "EQ reset command received");
    } else if (strncmp(cmd, "VOL:", 4) == 0) {
        ESP_LOGI(TAG, "Volume command = %s", cmd + 4);
    } else if (strncmp(cmd, "MODE:", 5) == 0) {
        ESP_LOGI(TAG, "Mode command = %s", cmd + 5);
    } else if (strncmp(cmd, "EQ", 2) == 0) {
        ESP_LOGI(TAG, "EQ command = %s", cmd);
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
    }
}

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
        if (len <= 0) return 0;

        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        if (len >= (int)sizeof(buffer)) len = sizeof(buffer) - 1;

        int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, len, NULL);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

        buffer[len] = '\0';
        handle_app_command(buffer);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

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
                ESP_LOGI(BLE_TAG, "Client connected, conn_handle=%u", current_conn_handle);
            } else {
                current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                notify_enabled = false;
                calibration_active = false;
                stop_histogram_stream();
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(BLE_TAG, "Client disconnected");
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
                if (!notify_enabled) {
                    calibration_active = false;
                    stop_histogram_stream();
                }
            }
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;

        default:
            return 0;
    }
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

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
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
    uart_driver_delete(UART_NUM_0);

    configure_spiffs();
    configure_psram();
    configure_spi();
    configure_i2s();
    initialize_pacer_timer(&sync_timer);

    sample_sem = xSemaphoreCreateBinary();
    if (sample_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create persistent sample semaphore");
        return;
    }

    gptimer_event_callbacks_t cbs = {
        .on_alarm = on_timer_alarm,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(sync_timer, &cbs, sample_sem));
    timer_callbacks_registered = true;

    esp_err_t ret = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(FFT_TAG, "FFT init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = 15000,
        .idle_core_mask = (1 << CORE0),
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&twdt_cfg);

    ret = nvs_flash_init();
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
// uwu
