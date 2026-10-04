#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct FamApu FamApu;
typedef struct FamMusic FamMusic;
typedef struct FamSfx FamSfx;
typedef struct FamPlayer FamPlayer;

typedef enum {
    FAM_PLAYBACK_STOPPED = 0,
    FAM_PLAYBACK_PLAYING = 1,
    FAM_PLAYBACK_PAUSED  = 2,
} FamPlaybackState;

#ifdef __cplusplus
extern "C" {
#endif

FAM_API size_t FAM_CALL fam_player_get_memory_required(void);
FAM_API size_t FAM_CALL fam_player_get_memory_alignment(void);
// The player takes exclusive ownership of the APU
// Once attached to a player, the APU shouldn't be driven directly
FAM_API FamResult FAM_CALL fam_player_init(FamPlayer** out_player, void* memory, FamApu* apu);
FAM_API void FAM_CALL fam_player_shutdown(FamPlayer* player);
FAM_API void FAM_CALL fam_player_run(FamPlayer* player, int sample_count, void* out_samples);

FAM_API FamResult FAM_CALL fam_player_play_music(FamPlayer* player, const FamMusic* music);
FAM_API void FAM_CALL fam_player_pause_music(FamPlayer* player);
FAM_API void FAM_CALL fam_player_resume_music(FamPlayer* player);
FAM_API void FAM_CALL fam_player_stop_music(FamPlayer* player);
FAM_API FamPlaybackState FAM_CALL fam_player_get_music_state(const FamPlayer* player);
FAM_API FamResult FAM_CALL fam_player_play_sfx(FamPlayer* player, const FamSfx* sfx);

#ifdef __cplusplus
}
#endif