#pragma once
#include <fam/common.h>
#include <stddef.h>
#include <stdint.h>

typedef struct FamMusic FamMusic;

FamResult fam_music_from_vgm_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer);
FamResult fam_music_from_vgm_file(FamMusic** out_music, const char* fname);
