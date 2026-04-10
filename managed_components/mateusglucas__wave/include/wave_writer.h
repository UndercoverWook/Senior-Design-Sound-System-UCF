#pragma once

#include "wave_common.h"

/**
 * @brief WAVE writer handle
 */
typedef struct wave_writer_t* wave_writer_handle_t;

/**
 * @brief Open a WAVE file in write mode
 * @param filename file name
 * @return WAVE writer handle
 * @note - If the file doesn't exist, it is created.
 * @note - If the file exists, it's content is cleared.
 */
wave_writer_handle_t wave_writer_open(const char *filename);

/**
 * @brief Write header to WAVE file
 * @param writer writer handle
 * @param header header informations
 * @return 0 on succes, -1 on fail
 */
int wave_write_header(wave_writer_handle_t writer, const wave_header_t *header);

/**
 * @brief Append raw samples data to WAVE file
 * @param writer writer handle
 * @param data buffer with the data to be written
 * @param size number of bytes to be written
 * @return number of written bytes
 * @note The header chunk size fields are not updated. 
 */
size_t wave_write_raw_data(wave_writer_handle_t writer, const uint8_t* data, size_t size);

/**
 * @brief Refresh header chunk size fields according to the current file size
 * @param writer writer handle
 * @return 0 on success, -1 on fail
 */
int wave_refresh_header_size_fields(wave_writer_handle_t writer);

/**
 * @brief Flush and sync file
 * @param writer writer handle
 * @return 0 on succes, -1 on fail
 */
int wave_writer_commit(wave_writer_handle_t writer);

/**
 * @brief Close WAVE file
 * @param writer writer handle
 * @return 0 on success, -1 on fail
 */
int wave_writer_close(wave_writer_handle_t writer);