#include "wave_reader.h"
#include "wave_common_priv.h"
#include <stdio.h>

struct wave_reader_t{
    FILE *f;
};

static size_t wave_read_bytes(wave_reader_handle_t reader, uint8_t *data, size_t pos, size_t size);

wave_reader_handle_t wave_reader_open(const char *filename){
    FILE* f = fopen(filename, "rb");
    if(f==NULL){
        return NULL;
    }

    struct wave_reader_t *reader = malloc(sizeof(struct wave_reader_t));
    if(reader==NULL){
        fclose(f);
        return NULL;
    }

    reader->f = f;

    return reader;
}

int wave_read_header(wave_reader_handle_t reader, wave_header_t *header){
    uint8_t num_channels_bytes[WAVE_NUM_CHANNELS_LEN];
    uint8_t bits_per_sample_bytes[WAVE_BITS_PER_SAMPLE_LEN];
    uint8_t sample_rate_bytes[WAVE_SAMPLE_RATE_LEN];
    uint8_t subchunk2_size_bytes[WAVE_SUBCHUNK_2_SIZE_LEN];

    size_t read_bytes = wave_read_bytes(reader, num_channels_bytes, WAVE_NUM_CHANNELS_POS, WAVE_NUM_CHANNELS_LEN);
    if(read_bytes!=WAVE_NUM_CHANNELS_LEN){
        return -1;
    }
    read_bytes = wave_read_bytes(reader, bits_per_sample_bytes, WAVE_BITS_PER_SAMPLE_POS, WAVE_BITS_PER_SAMPLE_LEN);
    if(read_bytes!=WAVE_BITS_PER_SAMPLE_LEN){
        return -1;
    }
    read_bytes = wave_read_bytes(reader, sample_rate_bytes, WAVE_SAMPLE_RATE_POS, WAVE_SAMPLE_RATE_LEN);
    if(read_bytes!=WAVE_SAMPLE_RATE_LEN){
        return -1;
    }
    read_bytes = wave_read_bytes(reader, subchunk2_size_bytes, WAVE_SUBCHUNK_2_SIZE_POS, WAVE_SUBCHUNK_2_SIZE_LEN);
    if(read_bytes!=WAVE_SUBCHUNK_2_SIZE_LEN){
        return -1;
    }

    uint16_t num_channels=0;
    uint16_t bits_per_sample=0;
    uint32_t sample_rate=0;
    uint32_t subchunk2_size=0;

    for(int i=0; i<WAVE_NUM_CHANNELS_LEN; i++){
        num_channels += (uint16_t) num_channels_bytes[i] << (8*i);
    }

    for(int i=0; i<WAVE_BITS_PER_SAMPLE_LEN; i++){
        bits_per_sample += (uint16_t) bits_per_sample_bytes[i] << (8*i);
    }

    for(int i=0; i<WAVE_SAMPLE_RATE_LEN; i++){
        sample_rate += (uint32_t) sample_rate_bytes[i] << (8*i);
    }

    for(int i=0; i<WAVE_SUBCHUNK_2_SIZE_LEN; i++){
        subchunk2_size += (uint32_t) subchunk2_size_bytes[i] << (8*i);
    }

    header->bytes_per_sample = bits_per_sample/8;
    header->n_channels = num_channels;
    header->sample_rate = sample_rate;
    header->samples_per_channel = subchunk2_size/(num_channels*(bits_per_sample/8));

    return 0;
}

size_t wave_read_raw_data(wave_reader_handle_t reader, uint8_t* data, size_t pos, size_t size){
    return wave_read_bytes(reader, data, WAVE_HEADER_SIZE + pos, size);
}

static size_t wave_read_bytes(wave_reader_handle_t reader, uint8_t *data, size_t pos, size_t size){
    int reti = fseek(reader->f, pos, SEEK_SET);
    if(reti!=0){
        return 0;
    }

    return fread(data, 1, size, reader->f);
}

int wave_reader_close(wave_reader_handle_t reader){
    int ret = fclose(reader->f);
    if(ret!=0){
        return ret;
    }

    free(reader);
    return 0;
}