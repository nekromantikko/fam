#include <fam/io.h>
#include <fam/internal/stream_types.h>
#include <fam/internal/stream_util.h>
#include <fam/internal/buffer_reader.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define FAM_MAGIC "FAM"
#define FAM_VERSION_MAJOR 1
#define FAM_VERSION_MINOR 0

typedef enum {
    FAM_USAGE_MUSIC = 0,
    FAM_USAGE_SFX,
} FamUsage;

static FamResult validate_header(BufferReader* reader, FamUsage expected_usage) {
    uint32_t magic;
    buffer_reader_read(reader, &magic, sizeof(uint32_t));
    if (reader->error ||
        memcmp(&magic, FAM_MAGIC, sizeof(FAM_MAGIC)) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint32_t version_major, version_minor;
    buffer_reader_read(reader, &version_major, sizeof(uint32_t));
    buffer_reader_read(reader, &version_minor, sizeof(uint32_t));
    if (reader->error ||
        version_major != FAM_VERSION_MAJOR ||
        version_minor > FAM_VERSION_MINOR) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint8_t usage;
    buffer_reader_read(reader, &usage, sizeof(uint8_t));
    if (reader->error ||
        usage != expected_usage) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    return FAM_SUCCESS;
}

FamResult fam_music_from_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer) {
    // TODO: An allocation-free version?
    if (out_music == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    BufferReader reader = buffer_reader_init(buffer, buffer_size);

    FamResult err = validate_header(&reader, FAM_USAGE_MUSIC);
    if (err != FAM_SUCCESS) {
        return err;
    }

    uint64_t channel_mask;
    buffer_reader_read(&reader, &channel_mask, sizeof(uint64_t));

    uint32_t bank_count;
    buffer_reader_read(&reader, &bank_count, sizeof(uint32_t));
    if (reader.error || 
        bank_count > MAX_DPCM_BANK_COUNT || 
        bank_count > SIZE_MAX / sizeof(DPCMSampleBank)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint64_t bank_offset;
    buffer_reader_read(&reader, &bank_offset, sizeof(uint64_t));
    if (reader.error || 
        bank_offset >= buffer_reader_size(&reader)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint32_t op_count;
    buffer_reader_read(&reader, &op_count, sizeof(uint32_t));
    if (reader.error || 
        op_count > MAX_STREAM_LENGTH || 
        op_count > SIZE_MAX / sizeof(StreamOperation)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint64_t stream_offset;
    buffer_reader_read(&reader, &stream_offset, sizeof(uint64_t));
    if (reader.error || 
        stream_offset >= buffer_reader_size(&reader)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint32_t loop_point;
    buffer_reader_read(&reader, &loop_point, sizeof(uint32_t));
    if (reader.error ||
        (loop_point != MUSIC_NO_LOOP && loop_point >= op_count)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    // Collect the bank sizes (a pass over the bank region) so we can size the allocation
    uint32_t bank_sizes[MAX_DPCM_BANK_COUNT];
    if (bank_count > 0) {
        buffer_reader_seek(&reader, bank_offset);
        for (size_t i = 0; i < bank_count && !reader.error; i++) {
            uint32_t bank_size;
            buffer_reader_read(&reader, &bank_size, sizeof(uint32_t));
            if (reader.error || 
                bank_size > buffer_reader_remaining(&reader) || 
                bank_size > MAX_DPCM_SAMPLE_BANK_SIZE) {
                return FAM_ERROR_INVALID_FORMAT;
            }
            bank_sizes[i] = bank_size;
            buffer_reader_skip(&reader, bank_size);
        }
    }

    if (reader.error) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    FamMusic* music;
    err = music_init(&music, channel_mask, loop_point, bank_count, bank_sizes, op_count);
    if (err != FAM_SUCCESS) {
        if (err == FAM_ERROR_OUT_OF_MEMORY) return err;
        return FAM_ERROR_INVALID_FORMAT;
    }

    // Read DPCM sample banks
    if (bank_count > 0) {
        buffer_reader_seek(&reader, bank_offset);
        for (size_t i = 0; i < bank_count; i++) {
            uint32_t bank_size;
            buffer_reader_read(&reader, &bank_size, sizeof(uint32_t));
            if (music->dpcm_sample_banks[i].data != NULL) {
                buffer_reader_read(&reader, music->dpcm_sample_banks[i].data, music->dpcm_sample_banks[i].size);
            }
        }
    }

    // Read stream ops
    if (op_count > 0) {
        buffer_reader_seek(&reader, stream_offset);
        for (size_t i = 0; i < op_count && !reader.error; i++) {
            StreamOperation op = {0};
            buffer_reader_read(&reader, &op.opcode, 1);
            buffer_reader_read(&reader, &op.data, 1);
            music->stream[i] = op;
        }
    }
    
    if (reader.error) {
        fam_music_free(music);
        return FAM_ERROR_INVALID_FORMAT;
    }

    *out_music = music;
    return FAM_SUCCESS;
}

void fam_music_free(FamMusic* music) {
    if (music == NULL) {
        return;
    }

    free(music);
}

FamResult fam_sfx_from_buffer(FamSfx** out_sfx, size_t buffer_size, const uint8_t* buffer) {
    // TODO: An allocation-free version?
    if (out_sfx == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    BufferReader reader = buffer_reader_init(buffer, buffer_size);

    FamResult header_result = validate_header(&reader, FAM_USAGE_SFX);
    if (header_result != FAM_SUCCESS) {
        return header_result;
    }

    uint64_t channel_id;
    buffer_reader_read(&reader, &channel_id, sizeof(uint64_t));
    if (reader.error ||
        channel_id >= SFX_CHANNEL_COUNT) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    buffer_reader_skip(&reader, sizeof(uint32_t) + sizeof(uint64_t)); // Skip over music-only sample stuff

    uint32_t op_count;
    buffer_reader_read(&reader, &op_count, sizeof(uint32_t));
    if (reader.error || 
        op_count > MAX_STREAM_LENGTH || 
        op_count > SIZE_MAX / sizeof(StreamOperation)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint64_t stream_offset;
    buffer_reader_read(&reader, &stream_offset, sizeof(uint64_t));
    if (reader.error || 
        stream_offset >= buffer_reader_size(&reader)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    size_t memory_size = sizeof(FamSfx) + op_count * sizeof(StreamOperation);

    void* memory = malloc(memory_size);
    if (memory == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    FamSfx* sfx = (FamSfx*)memory;
    sfx->channel_id = (uint8_t)channel_id;
    sfx->stream_op_count = op_count;
    sfx->stream = NULL;

    uint8_t* mem_pos = (uint8_t*)memory + sizeof(FamSfx);

    // Read stream ops
    if (op_count > 0) {
        sfx->stream = (StreamOperation*)((uint8_t*)memory + sizeof(FamSfx));
    
        buffer_reader_seek(&reader, stream_offset);
        for (size_t i = 0; i < op_count && !reader.error; i++) {
            StreamOperation op = {0};
            buffer_reader_read(&reader, &op.opcode, 1);
            buffer_reader_read(&reader, &op.data, 1);
            sfx->stream[i] = op;
        }
    }

    if (reader.error) {
        free(memory);
        return FAM_ERROR_INVALID_FORMAT;
    }

    *out_sfx = sfx;
    return FAM_SUCCESS;
}

void fam_sfx_free(FamSfx* sfx) {
    if (sfx == NULL) {
        return;
    }

    free(sfx);
}