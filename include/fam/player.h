#pragma once
#include <fam/common.h>
#include <stdint.h>

typedef struct FamApu FamApu;
typedef struct FamMusic FamMusic;
typedef struct FamPlayer FamPlayer;

FamResult fam_player_init(uint32_t sample_rate, FamPlayer** out_player);
void fam_player_free(FamPlayer* player);
void fam_player_process_samples(FamPlayer* player, int count, void* out_samples);

void fam_player_play_music(FamPlayer* player, const FamMusic* music);
void fam_player_pause_music(FamPlayer* player);
void fam_player_resume_music(FamPlayer* player);
void fam_player_stop_music(FamPlayer* player);