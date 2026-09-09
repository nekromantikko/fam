#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;

FAM_API FamResult fam_music_from_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer);
FAM_API void fam_music_free(FamMusic* music);
FAM_API FamMachine fam_music_get_machine(const FamMusic* music);

FAM_API FamResult fam_sfx_from_buffer(FamSfx** out_sfx, size_t buffer_size, const uint8_t* buffer);
FAM_API void fam_sfx_free(FamSfx* sfx);
FAM_API FamMachine fam_sfx_get_machine(const FamSfx* sfx);