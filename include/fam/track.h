#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    FAM_TRACK_USAGE_MUSIC = 0,
    FAM_TRACK_USAGE_SFX,
} FamTrackUsage;

typedef struct FamTrack FamTrack;

#ifdef __cplusplus
extern "C" {
#endif

// Tracks are loaded in place, so the buffer must stay alive for the lifetime of the track and not modified afterwards
FAM_API FamResult FAM_CALL fam_track_from_buffer(const FamTrack** out_track, size_t buffer_size, const void* buffer);
FAM_API FamRegion FAM_CALL fam_track_get_region(const FamTrack* track);
FAM_API FamTrackUsage FAM_CALL fam_track_get_usage(const FamTrack* track);
FAM_API uint64_t FAM_CALL fam_track_get_channel_bits(const FamTrack* track);
// Calling this with a music track results in FAM_CHANNEL_NONE
FAM_API FamChannel FAM_CALL fam_track_get_sfx_channel(const FamTrack* track);

#ifdef __cplusplus
}
#endif