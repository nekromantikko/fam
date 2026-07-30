#pragma once
#include <fam/common.h>
#include <stddef.h>

typedef struct FamFtModule FamFtModule;
typedef struct FamMusic FamMusic;

FamResult fam_ftmodule_from_file(FamFtModule** out_module, const char* fname);
size_t fam_ftmodule_track_count(const FamFtModule* module);
FamResult fam_music_from_ftmodule_track(FamMusic** out_music, const FamFtModule* module, size_t track_idx);
void fam_ftmodule_free(FamFtModule* module);