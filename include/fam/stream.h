#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;

#ifdef __cplusplus
extern "C" {
#endif

// Music is loaded in place, so the buffer must stay alive for the lifetime of the music track and not modified afterwards
FAM_API FamResult FAM_CALL fam_music_from_buffer(const FamMusic** out_music, size_t buffer_size, const void* buffer);
FAM_API FamRegion FAM_CALL fam_music_get_region(const FamMusic* music);

// See above comment
FAM_API FamResult FAM_CALL fam_sfx_from_buffer(const FamSfx** out_sfx, size_t buffer_size, const void* buffer);
FAM_API FamRegion FAM_CALL fam_sfx_get_region(const FamSfx* sfx);

#ifdef __cplusplus
}
#endif