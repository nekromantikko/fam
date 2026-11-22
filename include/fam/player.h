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
FAM_API void FAM_CALL fam_player_run(FamPlayer* player, size_t sample_count, void* out_samples);

// The player only references a stream's buffer while that stream is playing. Once the call
// that ends it returns (a stop, replacing it with another stream, or reaching its end during
// fam_player_run), the player never touches that buffer again.
// To free a stream from another thread while audio is running, stop it on the thread that owns
// the player, and only free it after that has happened. See examples/sdl_demo.

FAM_API FamResult FAM_CALL fam_player_play_music(FamPlayer* player, const FamMusic* music);
FAM_API void FAM_CALL fam_player_pause_music(FamPlayer* player);
FAM_API void FAM_CALL fam_player_resume_music(FamPlayer* player);
FAM_API void FAM_CALL fam_player_stop_music(FamPlayer* player);
// Returns NULL if no music is playing
FAM_API const FamMusic* FAM_CALL fam_player_get_music(const FamPlayer* player);
FAM_API FamPlaybackState FAM_CALL fam_player_get_music_state(const FamPlayer* player);

// Plays on the channel given by fam_sfx_get_channel, replacing any sound effect already playing there
FAM_API FamResult FAM_CALL fam_player_play_sfx(FamPlayer* player, const FamSfx* sfx);
// Stops the sound effect on the channel, if any, and hands the channel back to the music
FAM_API void FAM_CALL fam_player_stop_sfx(FamPlayer* player, FamChannel channel);
FAM_API void FAM_CALL fam_player_stop_all_sfx(FamPlayer* player);
// Returns NULL if no sound effect is playing on the channel, or if the channel can't play sound effects
FAM_API const FamSfx* FAM_CALL fam_player_get_sfx(const FamPlayer* player, FamChannel channel);

#ifdef __cplusplus
}
#endif