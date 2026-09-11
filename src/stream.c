#include <fam/stream.h>
#include <fam/internal/stream_types.h>
#include <fam/internal/buffer_reader.h>
#include <stdbool.h>
#include <string.h>
#include <stdalign.h>

#define FAM_MAGIC "FAM"
#define FAM_STREAM_VERSION_MAJOR 1
#define FAM_STREAM_VERSION_MINOR 0

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

_Static_assert(alignof(FamMusic) >= alignof(DPCMSampleBank), "FamMusic has to be the most strictly aligned type in the block");
_Static_assert(alignof(StreamOperation) == 1, "Stream operations must be byte-aligned");

// The layout is one block: the struct, then the bank array, then the bank data, then the stream.
// The bank array starts right after the struct, so the struct's size has to leave it aligned.
// Bank data and stream operations are byte aligned, so they can follow anything
_Static_assert(sizeof(FamMusic) % alignof(DPCMSampleBank) == 0, "sizeof(FamMusic) must be a multiple of alignof(DPCMSampleBank)!");

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
        out->version_major != FAM_STREAM_VERSION_MAJOR ||
        out->version_minor > FAM_STREAM_VERSION_MINOR) {
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

    // NOTE: Channels outside the ones we know about are not an error in the file, they're
    // channels of an expansion chip this version doesn't support yet. Refusing the whole stream
    // is better than playing it with the expansion channels silently missing.
    if (out->usage == FAM_USAGE_MUSIC &&
        (out->channel_id_mask & ~CHANNEL_MASK_ALL) != 0) {
        return FAM_ERROR_UNSUPPORTED_FEATURE;
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

FamResult fam_music_get_memory_required(size_t buffer_size, const uint8_t* buffer, size_t* out_size) {
    if (buffer == NULL || out_size == NULL) {
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

    *out_size = memory_size;
    return FAM_SUCCESS;
}

size_t fam_music_get_memory_alignment(void) {
    // NOTE: FamMusic is guaranteed to have the largest alignment, see assert at the top of the file
    return alignof(FamMusic);
}

FamResult fam_music_from_buffer(FamMusic** out_music, void* memory, size_t buffer_size, const uint8_t* buffer) {
    if (out_music == NULL || buffer == NULL || memory == NULL) {
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
        return FAM_ERROR_INVALID_FORMAT;
    }

    *out_music = music;
    return FAM_SUCCESS;
}

FamMachine fam_music_get_machine(const FamMusic* music) {
    return (FamMachine)music->machine;
}

FamResult fam_sfx_get_memory_required(size_t buffer_size, const uint8_t* buffer, size_t* out_size) {
    if (buffer == NULL || out_size == NULL) {
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

    *out_size = sizeof(FamSfx) + header.stream_length * sizeof(StreamOperation);
    return FAM_SUCCESS;
}

size_t fam_sfx_get_memory_alignment(void) {
    return alignof(FamSfx);
}

FamResult fam_sfx_from_buffer(FamSfx** out_sfx, void* memory, size_t buffer_size, const uint8_t* buffer) {
    if (out_sfx == NULL || buffer == NULL || memory == NULL) {
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

    FamSfx* sfx = (FamSfx*)memory;
    sfx->channel_id = (uint8_t)header.channel_id_mask;
    sfx->machine = header.machine;
    sfx->stream_op_count = header.stream_length;
    sfx->stream = NULL;

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
        return FAM_ERROR_INVALID_FORMAT;
    }

    *out_sfx = sfx;
    return FAM_SUCCESS;
}

FamMachine fam_sfx_get_machine(const FamSfx* sfx) {
    return (FamMachine)sfx->machine;
}