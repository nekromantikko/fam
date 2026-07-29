#pragma once
#include <fam/common.h>
#include <stddef.h>

typedef struct FamFtModule FamFtModule;

FamResult fam_ftmodule_from_file(FamFtModule** out_module, const char* fname);
size_t fam_ftmodule_track_count(const FamFtModule* module);
void fam_ftmodule_free(FamFtModule* module);