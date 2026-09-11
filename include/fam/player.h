#pragma once
#include <fam/common.h>
#include <stdint.h>

typedef struct FamApu FamApu;
typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;
typedef struct FamPlayer FamPlayer;

typedef struct FamPlayerConfig {
    uint32_t sample_rate;
    FamAudioFormat format;
} FamPlayerConfig;

FAM_API FamResult fam_player_get_memory_required(const FamPlayerConfig* config, size_t* out_size);
FAM_API size_t fam_player_get_memory_alignment(void);
FAM_API FamResult fam_player_init(FamPlayer** out_player, void* memory, FamApu* apu, const FamPlayerConfig* config);
FAM_API void fam_player_shutdown(FamPlayer* player);
FAM_API FamResult fam_player_process_samples(FamPlayer* player, int count, void* out_samples);

// NOTE: Music and sfx can only be played on an APU matching their machine (NTSC/PAL), otherwise
// FAM_ERROR_MACHINE_MISMATCH is returned and the player is left untouched
FAM_API FamResult fam_player_play_music(FamPlayer* player, const FamMusic* music);
FAM_API void fam_player_pause_music(FamPlayer* player);
FAM_API void fam_player_resume_music(FamPlayer* player);
FAM_API void fam_player_stop_music(FamPlayer* player);
FAM_API FamResult fam_player_play_sfx(FamPlayer* player, const FamSfx* sfx);