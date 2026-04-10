#include "wave_common.h"
#include "wave_common_priv.h"

#include <string.h>

void wave_print_header(const wave_header_t *header, uint8_t *buffer){
    uint32_t byte_rate = header->n_channels * header->bytes_per_sample * header->sample_rate;
    uint16_t block_align = header->n_channels * header->bytes_per_sample;
    uint16_t bits_per_sample = 8 * header->bytes_per_sample;

    uint32_t subchunk2_size = header->n_channels * header->bytes_per_sample * header->samples_per_channel;
    uint32_t chunk_size = subchunk2_size + WAVE_HEADER_SIZE - 8;

    // http://soundfile.sapp.org/doc/WaveFormat/
    uint8_t header_data[WAVE_HEADER_SIZE] = {
        /* RIFF chunk descriptor */
         'R',  'I',  'F',  'F', // ChunkID
        BYTE(chunk_size, 0), BYTE(chunk_size, 1), BYTE(chunk_size, 2), BYTE(chunk_size, 3), // ChunkSize
         'W',  'A',  'V',  'E', // Format

        /* fmt sub-chunk */
         'f',  'm',  't',  ' ',                                   // Subchunk1ID
        0x10, 0x00, 0x00, 0x00,                                   // Subchunk1Size (16, little-endian)
        0x01, 0x00,                                               // AudioFormat (PCM = 1, little-endian)
        BYTE(header->n_channels, 0), BYTE(header->n_channels, 1), // NumChannels (little-endian)
        BYTE(header->sample_rate, 0), BYTE(header->sample_rate, 1),   // SampleRate 0:1 (little-endian)
        BYTE(header->sample_rate, 2), BYTE(header->sample_rate, 3),   // SampleRate 2:3 (little-endian)
        BYTE(byte_rate, 0), BYTE(byte_rate, 1),                   // ByteRate 0:1 (little-endian)
        BYTE(byte_rate, 2), BYTE(byte_rate, 3),                   // ByteRate 2:3 (little-endian)       
        BYTE(block_align, 0), BYTE(block_align, 1),               // BlockAlign (little-endian) 
        BYTE(bits_per_sample, 0), BYTE(bits_per_sample, 1),       // BitsPerSample (little-endian)

        /* data sub-chunk */
         'd',  'a',  't',  'a', // Subchunk2ID
        BYTE(subchunk2_size, 0), BYTE(subchunk2_size, 1), BYTE(subchunk2_size, 2), BYTE(subchunk2_size, 3), // Subchunk2Size
    };

    memcpy(buffer, header_data, sizeof(header_data));
}