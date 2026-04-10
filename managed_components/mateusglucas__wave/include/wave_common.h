#pragma once

#include <stdint.h>
#include <stdio.h>

#define WAVE_HEADER_SIZE             (44)

/**
 * @brief Header information struct
 */
typedef struct{
    uint16_t n_channels;
    uint16_t sample_rate;
    uint32_t samples_per_channel;
    uint16_t bytes_per_sample;
}wave_header_t;

/**
 * @brief Print header bytes to buffer
 * @param header header information
 * @param buffer buffer to receive header bytes
 * @note - Buffer length should be at least WAVE_HEADER_SIZE
 */
void wave_print_header(const wave_header_t *header, uint8_t *buffer);