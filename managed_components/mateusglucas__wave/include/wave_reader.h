#pragma once

#include "wave_common.h"

/**
 * @brief WAVE reader handle
 */
typedef struct wave_reader_t* wave_reader_handle_t;

/**
 * @brief Open a WAVE file in read mode
 * @param filename file name
 * @return WAVE reader handle
 */
wave_reader_handle_t wave_reader_open(const char *filename);

/**
 * @brief Read WAVE file header
 * @param reader reader handle
 * @param header pointer to struct to store header informations
 * @return 0 on success, -1 on fail
 */
int wave_read_header(wave_reader_handle_t reader, wave_header_t *header);

/**
 * @brief Read raw samples bytes from WAVE file
 * @param reader reader handle
 * @param data pointer to buffer to store read data
 * @param pos offset in bytes from begining of samples data to start to read
 * @param size bytes to read
 * @return number of read bytes
 */
size_t wave_read_raw_data(wave_reader_handle_t reader, uint8_t* data, size_t pos, size_t size);

/**
 * @brief Close WAVE file
 * @param reader reader handle
 * @return 0 on succes, -1 on fail
 */
int wave_reader_close(wave_reader_handle_t reader);