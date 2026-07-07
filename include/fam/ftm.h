#pragma once
#include <fam/common.h>
#include <stddef.h>

typedef struct FamMusic FamMusic;

FamResult fam_load_music_ftm(const char* fname, size_t* out_count, FamMusic** out_tracks);