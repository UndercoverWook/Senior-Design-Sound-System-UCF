#pragma once

#define BYTE(val, n) ((uint8_t) ((val >> (8*n)) & 0xFF))

#define WAVE_CHUNK_SIZE_POS          (4)
#define WAVE_SUBCHUNK_2_SIZE_POS     (40)
#define WAVE_NUM_CHANNELS_POS        (22)
#define WAVE_SAMPLE_RATE_POS         (24)
#define WAVE_BITS_PER_SAMPLE_POS     (34)

#define WAVE_CHUNK_SIZE_LEN          (4)
#define WAVE_SUBCHUNK_2_SIZE_LEN     (4)
#define WAVE_NUM_CHANNELS_LEN        (2)
#define WAVE_SAMPLE_RATE_LEN         (4)
#define WAVE_BITS_PER_SAMPLE_LEN     (2)