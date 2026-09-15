#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;

#ifdef __cplusplus
extern "C" {
#endif

FAM_API FamResult FAM_CALL fam_music_get_memory_required(size_t buffer_size, const uint8_t* buffer, size_t* out_size);
FAM_API size_t FAM_CALL fam_music_get_memory_alignment(void);
FAM_API FamResult FAM_CALL fam_music_from_buffer(FamMusic** out_music, void* memory, size_t buffer_size, const uint8_t* buffer);
FAM_API FamRegion FAM_CALL fam_music_get_region(const FamMusic* music);
FAM_API void FAM_CALL fam_music_free(FamMusic* music);

FAM_API FamResult FAM_CALL fam_sfx_get_memory_required(size_t buffer_size, const uint8_t* buffer, size_t* out_size);
FAM_API size_t FAM_CALL fam_sfx_get_memory_alignment(void);
FAM_API FamResult FAM_CALL fam_sfx_from_buffer(FamSfx** out_sfx, void* memory, size_t buffer_size, const uint8_t* buffer);
FAM_API FamRegion FAM_CALL fam_sfx_get_region(const FamSfx* sfx);
FAM_API void FAM_CALL fam_sfx_free(FamSfx* sfx);

#ifdef __cplusplus
}
#endif