#include <fam/io.h>
#include <fam/internal/stream_types.h>
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

    FAM_USAGE_COUNT
} FamUsage;

typedef struct {
    uint32_t version_major;
    uint32_t version_minor;
    uint8_t usage;
    uint64_t channel_id_mask; // Depending on usage, a bitmask for music or channel ID for sfx
    uint32_t music_dpcm_bank_count;
    uint64_t music_dpcm_bank_offset; // Absolute
    uint32_t stream_length; // Length in operations
    uint64_t stream_offset; // Absolute
    uint32_t music_loop_point;
    uint8_t machine;
} FamHeader;

static FamResult parse_header(BufferReader* reader, FamHeader* out) {
    uint32_t magic;
    buffer_reader_read(reader, &magic, sizeof(uint32_t));
    if (reader->error ||
        memcmp(&magic, FAM_MAGIC, sizeof(FAM_MAGIC)) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    buffer_reader_read(reader, &out->version_major, sizeof(uint32_t));
    buffer_reader_read(reader, &out->version_minor, sizeof(uint32_t));
    if (reader->error ||
        out->version_major != FAM_VERSION_MAJOR ||
        out->version_minor > FAM_VERSION_MINOR) {
        return FAM_ERROR_UNSUPPORTED_VERSION;
    }

    buffer_reader_read(reader, &out->usage, sizeof(uint8_t));
    if (reader->error ||
        out->usage >= FAM_USAGE_COUNT) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    buffer_reader_read(reader, &out->channel_id_mask, sizeof(uint64_t));
    if (reader->error ||
        (out->usage == FAM_USAGE_SFX && 
        out->channel_id_mask >= SFX_CHANNEL_COUNT)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if (out->usage == FAM_USAGE_MUSIC) {
        buffer_reader_read(reader, &out->music_dpcm_bank_count, sizeof(uint32_t));
        if (reader->error || 
            out->music_dpcm_bank_count > MAX_DPCM_BANK_COUNT || 
            out->music_dpcm_bank_count > SIZE_MAX / sizeof(DPCMSampleBank)) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        buffer_reader_read(reader, &out->music_dpcm_bank_offset, sizeof(uint64_t));
        if (reader->error || 
            out->music_dpcm_bank_offset >= buffer_reader_size(reader)) {
            return FAM_ERROR_INVALID_FORMAT;
        }
    } else {
        buffer_reader_skip(reader, sizeof(uint32_t) + sizeof(uint64_t));
        out->music_dpcm_bank_count = 0;
        out->music_dpcm_bank_offset = 0;
    }

    buffer_reader_read(reader, &out->stream_length, sizeof(uint32_t));
    if (reader->error || 
        out->stream_length > MAX_STREAM_LENGTH || 
        out->stream_length > SIZE_MAX / sizeof(StreamOperation)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    buffer_reader_read(reader, &out->stream_offset, sizeof(uint64_t));
    if (reader->error || 
        out->stream_offset >= buffer_reader_size(reader)) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if (out->usage == FAM_USAGE_MUSIC) {
        buffer_reader_read(reader, &out->music_loop_point, sizeof(uint32_t));
        if (reader->error ||
            (out->music_loop_point != MUSIC_NO_LOOP && out->music_loop_point >= out->stream_length)) {
            return FAM_ERROR_INVALID_FORMAT;
        }
    } else {
        buffer_reader_skip(reader, sizeof(uint32_t));
        out->music_loop_point = MUSIC_NO_LOOP;
    }

    buffer_reader_read(reader, &out->machine, sizeof(uint8_t));
    if (reader->error || out->machine > FAM_MACHINE_PAL) {
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

    FamHeader header;
    FamResult header_result = parse_header(&reader, &header);
    if (header_result != FAM_SUCCESS) {
        return header_result;
    }

    if (header.usage != FAM_USAGE_MUSIC) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    // Determine required memory size
    // NOTE: sizeof(FamMusic) must be a multiple of alignof(DPCMSampleBank)!
    // As long as sizeof(FamMusic) is a multiple of 8, this holds
    size_t memory_size = sizeof(FamMusic) + header.music_dpcm_bank_count * sizeof(DPCMSampleBank) + header.stream_length * sizeof(StreamOperation);

    if (header.music_dpcm_bank_count > 0) {
        buffer_reader_seek(&reader, header.music_dpcm_bank_offset);
        for (size_t i = 0; i < header.music_dpcm_bank_count && !reader.error; i++) {
            uint32_t bank_size;
            buffer_reader_read(&reader, &bank_size, sizeof(uint32_t));
            if (reader.error || 
                bank_size > buffer_reader_remaining(&reader) || 
                bank_size > MAX_DPCM_SAMPLE_BANK_SIZE ||
                bank_size > SIZE_MAX - memory_size) {
                return FAM_ERROR_INVALID_FORMAT;
            }
            memory_size += bank_size;
            buffer_reader_skip(&reader, bank_size);
        }
    }

    if (reader.error) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    void* memory = malloc(memory_size);
    if (memory == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    FamMusic* music = (FamMusic*)memory;
    music->channel_mask = header.channel_id_mask;
    music->dpcm_sample_bank_count = header.music_dpcm_bank_count;
    music->dpcm_sample_banks = NULL;
    music->stream_op_count = header.stream_length;
    music->stream = NULL;
    music->loop_point = header.music_loop_point;
    music->machine = header.machine;

    uint8_t* mem_pos = (uint8_t*)memory + sizeof(FamMusic);

    // Read DPCM sample banks
    if (header.music_dpcm_bank_count > 0) {
        music->dpcm_sample_banks = (DPCMSampleBank*)mem_pos;
        mem_pos += sizeof(DPCMSampleBank) * header.music_dpcm_bank_count;

        buffer_reader_seek(&reader, header.music_dpcm_bank_offset);
        for (size_t i = 0; i < header.music_dpcm_bank_count && !reader.error; i++) {
            DPCMSampleBank* bank = &music->dpcm_sample_banks[i];
            buffer_reader_read(&reader, &bank->size, sizeof(uint32_t));
            
            if (bank->size == 0) {
                bank->data = NULL;
            } else {
                bank->data = mem_pos;
                buffer_reader_read(&reader, bank->data, bank->size);
                mem_pos += bank->size;
            }
        }
    }

    // Read stream ops
    if (header.stream_length > 0) {
        music->stream = (StreamOperation*)mem_pos;
    
        buffer_reader_seek(&reader, header.stream_offset);
        for (size_t i = 0; i < header.stream_length && !reader.error; i++) {
            StreamOperation op = {0};
            buffer_reader_read(&reader, &op.opcode, 1);
            buffer_reader_read(&reader, &op.data, 1);
            music->stream[i] = op;
        }
    }
    
    if (reader.error) {
        free(memory);
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

    FamHeader header;
    FamResult header_result = parse_header(&reader, &header);
    if (header_result != FAM_SUCCESS) {
        return header_result;
    }

    if (header.usage != FAM_USAGE_SFX) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    size_t memory_size = sizeof(FamSfx) + header.stream_length * sizeof(StreamOperation);

    void* memory = malloc(memory_size);
    if (memory == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    FamSfx* sfx = (FamSfx*)memory;
    sfx->channel_id = (uint8_t)header.channel_id_mask;
    sfx->stream_op_count = header.stream_length;
    sfx->stream = NULL;

    uint8_t* mem_pos = (uint8_t*)memory + sizeof(FamSfx);

    // Read stream ops
    if (header.stream_length > 0) {
        sfx->stream = (StreamOperation*)((uint8_t*)memory + sizeof(FamSfx));
    
        buffer_reader_seek(&reader, header.stream_offset);
        for (size_t i = 0; i < header.stream_length && !reader.error; i++) {
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