#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;

FAM_API FamResult fam_music_get_memory_required(size_t buffer_size, const uint8_t* buffer, size_t* out_size);
FAM_API size_t fam_music_get_memory_alignment(void);
FAM_API FamResult fam_music_from_buffer(FamMusic** out_music, void* memory, size_t buffer_size, const uint8_t* buffer);
FAM_API FamMachine fam_music_get_machine(const FamMusic* music);

FAM_API FamResult fam_sfx_get_memory_required(size_t buffer_size, const uint8_t* buffer, size_t* out_size);
FAM_API size_t fam_sfx_get_memory_alignment(void);
FAM_API FamResult fam_sfx_from_buffer(FamSfx** out_sfx, void* memory, size_t buffer_size, const uint8_t* buffer);
FAM_API FamMachine fam_sfx_get_machine(const FamSfx* sfx);