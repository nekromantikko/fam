#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;

FamResult fam_music_from_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer);
uint8_t fam_music_get_region(const FamMusic* music);
void fam_music_free(FamMusic* music);

FamResult fam_sfx_from_buffer(FamSfx** out_sfx, size_t buffer_size, const uint8_t* buffer);
uint8_t fam_sfx_get_region(const FamSfx* sfx);
void fam_sfx_free(FamSfx* sfx);