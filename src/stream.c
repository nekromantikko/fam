#include <fam/stream.h>
#include <fam/internal/stream.h>
#include <stdbool.h>
#include <string.h>

static FamResult validate_format(size_t buffer_size, const uint8_t* buffer) {
    if (buffer_size < STREAM_HEADER_SIZE) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    const char* magic = (const char*)(buffer + STREAM_HEADER_FIELD_MAGIC);
    if (memcmp(magic, STREAM_MAGIC, sizeof(STREAM_MAGIC)) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    const uint16_t version_major = read_u16_le(buffer + STREAM_HEADER_FIELD_VERSION_MAJOR);
    const uint16_t version_minor = read_u16_le(buffer + STREAM_HEADER_FIELD_VERSION_MINOR);
    if (version_major != STREAM_VERSION_MAJOR ||
        version_minor > STREAM_VERSION_MINOR) {
        return FAM_ERROR_UNSUPPORTED_VERSION;
    }

    const size_t total_size = (size_t)read_u32_le(buffer + STREAM_HEADER_FIELD_EOF_OFFSET);
    if (total_size < STREAM_HEADER_SIZE || total_size > buffer_size) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    return FAM_SUCCESS;
}

static FamResult validate_common(const StreamInfo* info, StreamUsage expected_usage) {
    if (info->usage != expected_usage ||
        info->region > FAM_REGION_PAL) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if ((info->channel_mask & ~CHANNEL_MASK_SUPPORTED) != 0) {
        return FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    if (info->op_data_offset > info->total_size || info->op_data_size > info->total_size - info->op_data_offset) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    return FAM_SUCCESS;
}

static FamResult validate_music(const StreamInfo* info) {
    FamResult result = validate_common(info, STREAM_USAGE_MUSIC);
    if (result != FAM_SUCCESS) {
        return result;
    }

    // Loop offset needs to either be zero (no loop) 
    // or be within the data block and aligned to an opcode
    if (info->music_loop_offset != 0) {
        if (info->music_loop_offset < info->op_data_offset ||
            info->music_loop_offset - info->op_data_offset >= info->op_data_size ||
            ((info->music_loop_offset - info->op_data_offset) & 1) != 0 ) {
            return FAM_ERROR_INVALID_FORMAT;
        }
    }

    if (info->music_dpcm_bank_count > MUSIC_MAX_DPCM_BANK_COUNT) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if (info->music_dpcm_bank_count == 0) {
        return FAM_SUCCESS;
    }

    if (info->music_dpcm_bank_data_offset > info->total_size || info->music_dpcm_bank_data_size > info->total_size - info->music_dpcm_bank_data_offset) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    return FAM_SUCCESS;
}

// A sound effect claims at most one channel
// An empty mask is allowed, but won't do anything
static FamResult validate_sfx(const StreamInfo* info) {
    FamResult result = validate_common(info, STREAM_USAGE_SFX);
    if (result != FAM_SUCCESS) {
        return result;
    }

    // More than one bit set
    if (info->channel_mask != 0 && (info->channel_mask & (info->channel_mask - 1)) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if ((info->channel_mask & ~CHANNEL_MASK_SFX) != 0) {
        return FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    return FAM_SUCCESS;
}

FamResult fam_music_from_buffer(const FamMusic** out_music, size_t buffer_size, const void* buffer) {
    if (out_music == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FamResult result = validate_format(buffer_size, buffer);
    if (result != FAM_SUCCESS) {
        return result;
    }

    const StreamInfo info = stream_get_info(buffer);

    result = validate_music(&info);
    if (result != FAM_SUCCESS) {
        return result;
    }

    *out_music = (const FamMusic*)buffer;
    return FAM_SUCCESS;
}

FamRegion fam_music_get_region(const FamMusic* music) {
    return (FamRegion)((const uint8_t*)music)[STREAM_HEADER_FIELD_REGION];
}

FamResult fam_sfx_from_buffer(const FamSfx** out_sfx, size_t buffer_size, const void* buffer) {
    if (out_sfx == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FamResult result = validate_format(buffer_size, buffer);
    if (result != FAM_SUCCESS) {
        return result;
    }

    const StreamInfo info = stream_get_info(buffer);

    result = validate_sfx(&info);
    if (result != FAM_SUCCESS) {
        return result;
    }

    *out_sfx = (const FamSfx*)buffer;
    return FAM_SUCCESS;
}

FamRegion fam_sfx_get_region(const FamSfx* sfx) {
    return (FamRegion)((const uint8_t*)sfx)[STREAM_HEADER_FIELD_REGION];
}
