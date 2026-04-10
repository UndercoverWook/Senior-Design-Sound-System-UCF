#include "wave_writer.h"
#include "wave_common_priv.h"
#include <stdio.h>
#include <unistd.h>

struct wave_writer_t{
    FILE *f;
};

static int wave_write_uint32(wave_writer_handle_t writer, long pos, uint32_t val);

wave_writer_handle_t wave_writer_open(const char *filename){
    FILE* f = fopen(filename, "wb");
    if(f==NULL){
        return NULL;
    }

    struct wave_writer_t *writer = malloc(sizeof(struct wave_writer_t));
    if(writer==NULL){
        fclose(f);
        return NULL;
    }

    writer->f = f;

    return writer;
}

int wave_write_header(wave_writer_handle_t writer, const wave_header_t *header){
    uint8_t header_data[WAVE_HEADER_SIZE];
    wave_print_header(header, header_data);

    int ret = fseek(writer->f, 0, SEEK_SET);
    if(ret!=0){
        return ret;
    }

    size_t written_bytes = fwrite(header_data, 1, sizeof(header_data), writer->f);
    if(written_bytes != sizeof(header_data)){
        return -1;
    }

    return 0;
}

size_t wave_write_raw_data(wave_writer_handle_t writer, const uint8_t* data, size_t size){
    int ret = fseek(writer->f, 0, SEEK_END);
    if(ret!=0){
        return 0;
    }

    long file_size = ftell(writer->f);
    if(file_size == -1L){
        return 0;
    }else if(file_size < WAVE_HEADER_SIZE){
        ret = fseek(writer->f, WAVE_HEADER_SIZE, SEEK_SET);
        if(ret!=0){
            return 0;
        }  
    }

    return fwrite(data, 1, size, writer->f);
}

int wave_refresh_header_size_fields(wave_writer_handle_t writer){
    int ret = fseek(writer->f, 0, SEEK_END);
    if(ret!=0){
        return ret;
    }
    long file_size = ftell(writer->f);
    if(file_size < WAVE_HEADER_SIZE){
        return -1;
    }

    uint32_t chunk_size = file_size - 8;
    ret = wave_write_uint32(writer, WAVE_CHUNK_SIZE_POS, chunk_size);
    if(ret!=0){
        return ret;
    }

    uint32_t subchunk_2_size = file_size - WAVE_HEADER_SIZE;
    ret = wave_write_uint32(writer, WAVE_SUBCHUNK_2_SIZE_POS, subchunk_2_size);
    if(ret!=0){
        return ret;
    }

    return 0;
}

static int wave_write_uint32(wave_writer_handle_t writer, long pos, uint32_t val){
    uint8_t bytes[sizeof(val)];

    for(int i=0; i<sizeof(val); i++){
        bytes[i] = BYTE(val, i);
    }

    int ret = fseek(writer->f, pos, SEEK_SET);
    if(ret!=0){
        return ret;
    }

    size_t written_bytes = fwrite(bytes, 1, sizeof(bytes), writer->f);
    if(written_bytes != sizeof(bytes)){
        return -1;
    }

    return 0;
}

int wave_writer_commit(wave_writer_handle_t writer){
    int ret = fflush(writer->f);
    if(ret!=0){
        return ret;
    }

    int fno = fileno(writer->f);
    if(fno==-1){
        return -1;
    }

    return fsync(fno);
}

int wave_writer_close(wave_writer_handle_t writer){
    int ret = fclose(writer->f);
    if(ret!=0){
        return ret;
    }

    free(writer);
    return 0;
}