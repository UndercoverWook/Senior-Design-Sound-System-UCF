/*
 * glb_params.h
 *
 *  Created on: Mar 21, 2026
 *      Author: matia
 */

#ifndef MAIN_MY_FUNCTIONS_GLB_PARAMS_H_
#define MAIN_MY_FUNCTIONS_GLB_PARAMS_H_

#include "esp_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "hal/spi_types.h"
#include "soc/gpio_num.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "esp_spiffs.h"
#include "esp_err.h"
#include "freertos/idf_additions.h"
#include "soc/clk_tree_defs.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_task_wdt.h"
#include "esp_log_timestamp.h"
#include "driver/gpio.h"
#include "driver/i2s_types.h"
#include "wave.h"
#include "wave_common.h"
#include "wave_reader.h"
#include "esp_rom_sys.h"
#include "hal/i2s_types.h"
#include "usb_device_uac.h"
#include "driver/uart.h"

// General parameters
#define SAMPLE_RATE         44100
#define TEST_DURATION       5
#define BUFFER_FRAMES       4096
#define BYTES_PER_SAMPLE    2
#define CHANNELS            2
#define FRAME_SIZE_BYTES    (BYTES_PER_SAMPLE * CHANNELS)
#define BUFFER_BYTES        (BUFFER_FRAMES * FRAME_SIZE_BYTES)
#define PACER_TIMER_HZ      10000000ULL
#define FFT_SIZE            2048
#define NUM_BINS            (FFT_SIZE / 2)
#define FREQ_START          20.0f
#define FREQ_END            20000.0f
#define AMPLITUDE           0x3FFFFF
#define CORE0               0
#define CORE1               1
#define TRANSACTION_LENGTH  16
#define N_SAMPLES           (SAMPLE_RATE * TEST_DURATION)
#define FFT_CACHE_PATH      "/storage/fft_cache.bin"
#define TASK_A_READY_BIT    BIT0
#define TASK_B_READY_BIT    BIT1
#define ALL_TASKS_READY     (TASK_A_READY_BIT | TASK_B_READY_BIT)

// GPIOs Declarations
static const gpio_num_t MCU_WAKE        = GPIO_NUM_1;
static const gpio_num_t I2S_RX_LINE     = GPIO_NUM_2;
static const gpio_num_t BUS_SENSE       = GPIO_NUM_6;
static const gpio_num_t HIGH_FAULT      = GPIO_NUM_7;
static const gpio_num_t BT_UART_TXD     = GPIO_NUM_8;
static const gpio_num_t BT_UART_RXD     = GPIO_NUM_9;
static const gpio_num_t ADC_CS_PIN      = GPIO_NUM_10;
static const gpio_num_t I2S_LRCLK_PIN   = GPIO_NUM_11;
static const gpio_num_t ADC_SCLK_PIN    = GPIO_NUM_12;
static const gpio_num_t ADC_MISO_PIN    = GPIO_NUM_13;
static const gpio_num_t I2S_TX_LINE     = GPIO_NUM_14;
static const gpio_num_t HIGH_MUTE       = GPIO_NUM_15;
static const gpio_num_t SUB_CLIP        = GPIO_NUM_16;
static const gpio_num_t SUB_FAULT       = GPIO_NUM_17;
static const gpio_num_t SUB_RST         = GPIO_NUM_18;
static const gpio_num_t USB_D_MINUS     = GPIO_NUM_19;
static const gpio_num_t USB_D_PLUS      = GPIO_NUM_20;
static const gpio_num_t I2S_BIT_CLK     = GPIO_NUM_21;
static const gpio_num_t BT_WAKE         = GPIO_NUM_48;

typedef enum {
    I2S_MODE_NONE = 0,
    I2S_MODE_WAV,
    I2S_MODE_AUDIO,
} i2s_mode_t;

// Handle Declarations
extern gptimer_handle_t    sync_timer;
extern spi_device_handle_t spi_hdl;
extern i2s_chan_handle_t   mcu_tx;
extern i2s_chan_handle_t   mcu_rx;
extern wave_reader_handle_t wav_hdl;
extern TaskHandle_t        task1_hdl;
extern TaskHandle_t        task2_hdl;
extern RingbufHandle_t     audio_ringbuf;
extern EventGroupHandle_t  sync_tasks;

extern bool                spi_initialized;
extern bool                spiffs_initialized;
extern volatile bool       play_test_running;
extern volatile bool       sample_task_running;
extern volatile bool       wav_task_running;
extern i2s_mode_t          current_i2s_mode;

// Function helpers:
extern float cal_values[256][2];
static const uint32_t STACK_DEPTH = 4096;

// TAGS:
extern const char *STORAGE_TAG;
extern const char *SPI_TAG;
extern const char *I2S_TAG;
extern const char *MIC_TAG;
extern const char *TEST_TAG;
extern const char *SAMPLING_TAG;
extern const char *TONE_TAG;
extern const char *WAV_TAG;
extern const char *EQ_TAG;
extern const char *BM83_TAG;

#ifdef __cplusplus
}
#endif

#endif /* MAIN_MY_FUNCTIONS_GLB_PARAMS_H_ */
