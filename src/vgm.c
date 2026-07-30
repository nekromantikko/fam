#include <fam/vgm.h>
#include <fam/internal/stream_types.h>
#include <fam/internal/buffer_reader.h>
#include <stdio.h>
#include <stdlib.h>

#define VGM_MAGIC "Vgm "
#define VGM_MIN_VERSION 0x00000161

static FamResult validate_header(BufferReader* reader) {
    uint32_t magic;
    buffer_reader_read(reader, &magic, sizeof(uint32_t));
    if (reader->error ||
        memcmp(&magic, VGM_MAGIC, 4) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    buffer_reader_seek(reader, 0x08);
    uint32_t version;
    buffer_reader_read(reader, &version, sizeof(uint32_t));
    if (reader->error || version < VGM_MIN_VERSION) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    printf("VGM version number = %lx\n", version);

    buffer_reader_seek(reader, 0x18);
    uint32_t sample_count;
    buffer_reader_read(reader, &sample_count, sizeof(uint32_t));
    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    printf("Total sample count = %lu\n", sample_count);

    buffer_reader_seek(reader, 0x34);
    uint32_t rel_data_offset;
    buffer_reader_read(reader, &rel_data_offset, sizeof(uint32_t));
    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    uint32_t abs_data_offset = 0x40; // Default
    if (version >= 0x00000150 && rel_data_offset != 0) {
        abs_data_offset = 0x34 + rel_data_offset;
    }

    printf("Absolute data offset = %lu\n", abs_data_offset);

    buffer_reader_seek(reader, 0x84);
    uint32_t nes_apu_clock;
    buffer_reader_read(reader, &nes_apu_clock, sizeof(uint32_t));
    if (reader->error || nes_apu_clock == 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    printf("NES APU clock = %lu\n", nes_apu_clock);

    return FAM_SUCCESS;
}

FamResult fam_music_from_vgm_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer) {
    if (out_music == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    BufferReader reader = buffer_reader_init(buffer, buffer_size);

    return validate_header(&reader);
}

FamResult fam_music_from_vgm_file(FamMusic** out_music, const char* fname) {
    if (out_music == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }
    
    FILE* file = fopen(fname, "rb");
    if (file == NULL) {
        return FAM_ERROR_IO;
    }

    fseek(file, 0, SEEK_END);
    size_t file_length = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_length < 0) {
        fclose(file);
        return FAM_ERROR_IO;
    }

    uint8_t* buffer = (uint8_t*)malloc(file_length);
    if (buffer == NULL) {
        fclose(file);
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    size_t read_len = fread(buffer, 1, file_length, file);
    fclose(file);

    if (read_len != file_length) {
        free(buffer);
        return FAM_ERROR_IO;
    }

    FamResult result = fam_music_from_vgm_buffer(out_music, file_length, buffer);
    free(buffer);

    return result;
}