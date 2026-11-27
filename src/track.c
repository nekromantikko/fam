#include <fam/track.h>
#include <fam/internal/track.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static FamResult validate_format(size_t buffer_size, const uint8_t* buffer) {
    if (buffer_size < TRACK_HEADER_SIZE) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    const char* magic = (const char*)(buffer + TRACK_HEADER_FIELD_MAGIC);
    if (memcmp(magic, TRACK_MAGIC, sizeof(TRACK_MAGIC)) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    const uint16_t version_major = read_u16_le(buffer + TRACK_HEADER_FIELD_VERSION_MAJOR);
    const uint16_t version_minor = read_u16_le(buffer + TRACK_HEADER_FIELD_VERSION_MINOR);
    if (version_major != TRACK_VERSION_MAJOR ||
        version_minor > TRACK_VERSION_MINOR) { // TODO?: Should I allow a larger minor version, actually?
        return FAM_ERROR_UNSUPPORTED_VERSION;
    }

    const size_t total_size = (size_t)read_u32_le(buffer + TRACK_HEADER_FIELD_EOF_OFFSET);
    if (total_size < TRACK_HEADER_SIZE || total_size > buffer_size) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    return FAM_SUCCESS;
}

static FamResult validate_common(const TrackInfo* info) {
    if (info->usage > FAM_TRACK_USAGE_SFX ||
        info->region > FAM_REGION_PAL) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if ((info->channel_mask & ~CHANNEL_MASK_SUPPORTED) != 0) {
        return FAM_ERROR_UNSUPPORTED_FEATURE;
    }

#if SIZE_MAX / TRACK_OP_SIZE < UINT32_MAX
    if (info->op_count > SIZE_MAX / TRACK_OP_SIZE) {
        return FAM_ERROR_INVALID_FORMAT;
    }
#endif

    if (info->op_data_offset > info->total_size || info->op_data_size > info->total_size - info->op_data_offset) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    return FAM_SUCCESS;
}

static FamResult validate_music(const TrackInfo* info) {
    // Loop offset needs to either be zero (no loop) 
    // or be within the data block and aligned to an opcode
    if (info->music_loop_offset != 0) {
        if (info->music_loop_offset < info->op_data_offset ||
            info->music_loop_offset - info->op_data_offset >= info->op_data_size ||
            ((info->music_loop_offset - info->op_data_offset) & 3) != 0 ) {
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

// A sound effect claims exactly one channel
static FamResult validate_sfx(const TrackInfo* info) {
    // No bits or more than one bit set
    if (info->channel_mask == 0 || (info->channel_mask & (info->channel_mask - 1)) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if ((info->channel_mask & ~CHANNEL_MASK_SFX) != 0) {
        return FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    return FAM_SUCCESS;
}

FamResult fam_track_from_buffer(const FamTrack** out_track, size_t buffer_size, const void* buffer) {
    if (out_track == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FamResult result = validate_format(buffer_size, buffer);
    if (result != FAM_SUCCESS) {
        return result;
    }

    const TrackInfo info = track_get_info(buffer);

    result = validate_common(&info);
    if (result != FAM_SUCCESS) {
        return result;
    }

    switch (info.usage) {
        case FAM_TRACK_USAGE_MUSIC:
            result = validate_music(&info);
            break;
        case FAM_TRACK_USAGE_SFX:
            result = validate_sfx(&info);
            break;
        default:
            return FAM_ERROR_INVALID_FORMAT; // Should be unreachable because validate_common checks for this already
    }
    if (result != FAM_SUCCESS) {
        return result;
    }

    *out_track = (const FamTrack*)buffer;
    return FAM_SUCCESS;
}

FamRegion fam_track_get_region(const FamTrack* track) {
    assert(track != NULL);
    return (FamRegion)((const uint8_t*)track)[TRACK_HEADER_FIELD_REGION];
}

FamTrackUsage fam_track_get_usage(const FamTrack* track) {
    assert(track != NULL);
    return (FamTrackUsage)((const uint8_t*)track)[TRACK_HEADER_FIELD_USAGE];
}

uint64_t fam_track_get_channel_bits(const FamTrack* track) {
    assert(track != NULL);
    return read_u64_le((const uint8_t*)track + TRACK_HEADER_FIELD_CHANNEL_MASK);
}

FamChannel fam_track_get_sfx_channel(const FamTrack* track) {
    assert(track != NULL);
    if (fam_track_get_usage(track) != FAM_TRACK_USAGE_SFX) {
        return FAM_CHANNEL_NONE;
    }

    const uint64_t channel_mask = fam_track_get_channel_bits(track);
    // Validated at load time to have exactly one bit set
    int channel = 0;
    while ((channel_mask >> channel & 1) == 0) {
        channel++;
    }
    return (FamChannel)channel;
}