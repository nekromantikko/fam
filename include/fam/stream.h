#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;

FamResult fam_music_from_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer);
void fam_music_free(FamMusic* music);
uint8_t fam_music_get_machine(const FamMusic* music);

FamResult fam_sfx_from_buffer(FamSfx** out_sfx, size_t buffer_size, const uint8_t* buffer);
void fam_sfx_free(FamSfx* sfx);
uint8_t fam_sfx_get_machine(const FamSfx* sfx);