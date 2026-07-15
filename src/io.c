#include <fam/io.h>
#include <fam/internal/stream_types.h>
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

// TODO: Move to utils
typedef struct {
    uint8_t* const start;
    uint8_t* pos;
    uint8_t* const end;
    bool error;
} BufferReader;

static inline BufferReader buffer_reader_init(const void* buffer, size_t size) {
    BufferReader reader = {
        .start = (uint8_t*)buffer,
        .pos = (uint8_t*)buffer,
        .end = (uint8_t*)buffer + size,
        .error = false
    };
    return reader;
}

static inline void buffer_reader_read(BufferReader* reader, void* dst, size_t count) {
    if (reader->error) {
        return;
    }

    // Trying to read past block end
    if (count > (size_t)(reader->end - reader->pos)) {
        reader->pos = reader->end;
        reader->error = true;
        return;
    }

    memcpy(dst, (void*)reader->pos, count);
    reader->pos += count;
}

static inline void buffer_reader_skip(BufferReader* reader, size_t count) {
    if (reader->error) {
        return;
    }

    // Trying to skip past block end
    if (count > (size_t)(reader->end - reader->pos)) {
        reader->pos = reader->end;
        reader->error = true;
        return;
    }

    reader->pos += count;
}

static inline void buffer_reader_seek(BufferReader* reader, size_t offset) {
    if (reader->error) {
        return;
    }

    if (offset > (size_t)(reader->end - reader->start)) {
        reader->pos = reader->end;
        reader->error = true;
        return;
    }

    reader->pos = reader->start + offset;
}

static inline size_t buffer_reader_size(const BufferReader* reader) {
    return (size_t)(reader->end - reader->start);
}

static inline size_t buffer_reader_remaining(const BufferReader* reader) {
    return (size_t)(reader->end - reader->pos);
}

FamResult fam_music_from_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer) {
    // TODO: An allocation-free version?
    if (out_music == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    BufferReader reader = buffer_reader_init(buffer, buffer_size);

    uint32_t magic;
    buffer_reader_read(&reader, &magic, sizeof(uint32_t));
    if (memcmp(&magic, FAM_MAGIC, sizeof(FAM_MAGIC)) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint32_t version_major, version_minor;
    buffer_reader_read(&reader, &version_major, sizeof(uint32_t));
    buffer_reader_read(&reader, &version_minor, sizeof(uint32_t));
    if (version_major > FAM_VERSION_MAJOR || 
        version_minor > FAM_VERSION_MINOR) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint8_t usage;
    buffer_reader_read(&reader, &usage, sizeof(uint8_t));
    if (reader.error || 
        usage != FAM_USAGE_MUSIC) {
        return FAM_ERROR_INVALID_FORMAT;
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
    if (reader.error) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    // Determine required memory size
    size_t memory_size = sizeof(FamMusic) + bank_count * sizeof(DPCMSampleBank) + op_count * sizeof(StreamOperation);

    if (bank_count > 0) {
        buffer_reader_seek(&reader, bank_offset);
        for (int i = 0; i < bank_count && !reader.error; i++) {
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
    music->channel_mask = channel_mask;
    music->dpcm_sample_bank_count = bank_count;
    music->dpcm_sample_banks = NULL;
    music->stream_op_count = op_count;
    music->stream = NULL;
    music->loop_point = loop_point;

    uint8_t* mem_pos = (uint8_t*)memory + sizeof(FamMusic);

    // Read DPCM sample banks
    if (bank_count > 0) {
        music->dpcm_sample_banks = (DPCMSampleBank*)mem_pos;
        mem_pos += sizeof(DPCMSampleBank) * bank_count;

        buffer_reader_seek(&reader, bank_offset);
        for (int i = 0; i < bank_count; i++) {
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
    if (op_count > 0) {
        music->stream = (StreamOperation*)mem_pos;
    
        buffer_reader_seek(&reader, stream_offset);
        for (size_t i = 0; i < op_count && !reader.error; i++) {
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

    uint32_t magic;
    buffer_reader_read(&reader, &magic, sizeof(uint32_t));
    if (memcmp(&magic, FAM_MAGIC, sizeof(FAM_MAGIC)) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint32_t version_major, version_minor;
    buffer_reader_read(&reader, &version_major, sizeof(uint32_t));
    buffer_reader_read(&reader, &version_minor, sizeof(uint32_t));
    if (version_major > FAM_VERSION_MAJOR || 
        version_minor > FAM_VERSION_MINOR) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint8_t usage;
    buffer_reader_read(&reader, &usage, sizeof(uint8_t));
    if (reader.error || 
        usage != FAM_USAGE_SFX) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint64_t channel_id;
    buffer_reader_read(&reader, &channel_id, sizeof(uint64_t));
    if (reader.error || 
        channel_id > UINT8_MAX) {
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