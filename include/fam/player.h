#pragma once
#include <fam/common.h>
#include <stdint.h>

typedef struct FamApu FamApu;
typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;
typedef struct FamPlayer FamPlayer;

#ifdef __cplusplus
extern "C" {
#endif

FAM_API FamResult FAM_CALL fam_player_init(FamPlayer** out_player, FamApu* apu, uint32_t sample_rate, uint8_t format);
FAM_API void FAM_CALL fam_player_free(FamPlayer* player);
FAM_API FamResult FAM_CALL fam_player_process_samples(FamPlayer* player, int count, void* out_samples);

FAM_API FamResult FAM_CALL fam_player_play_music(FamPlayer* player, const FamMusic* music);
FAM_API void FAM_CALL fam_player_pause_music(FamPlayer* player);
FAM_API void FAM_CALL fam_player_resume_music(FamPlayer* player);
FAM_API void FAM_CALL fam_player_stop_music(FamPlayer* player);
FAM_API FamResult FAM_CALL fam_player_play_sfx(FamPlayer* player, const FamSfx* sfx);

#ifdef __cplusplus
}
#endif