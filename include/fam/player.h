#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>

typedef struct FamApu FamApu;
typedef struct FamTrack FamTrack;
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

// The player only references a track's buffer while that track is playing or queued. Once the
// call that ends it returns (a stop, replacing it with another track, or reaching its end during
// fam_player_run), the player never touches that buffer again.
// To free a track while audio is running, call fam_player_stop_track on the thread that owns the
// player, and only free the buffer after that call has returned. Don't read player state from
// another thread to find out, see examples/sdl_demo for one way to get an acknowledgement back.

// Starts the music from the beginning, replacing the current music and clearing the queue
FAM_API FamResult FAM_CALL fam_player_play_music(FamPlayer* player, const FamTrack* music);
// Starts the music when the current music ends by itself, with no gap. Looping music never ends
// by itself, so music queued after it never starts (see fam_track_has_loop).
// Starts immediately if no music is playing. Replaces anything already queued, and NULL clears the queue.
// fam_player_play_music, fam_player_stop_music and fam_player_stop_track also clear the queue.
// Checks the track the same way as fam_player_play_music, so errors are reported when queueing
FAM_API FamResult FAM_CALL fam_player_queue_music(FamPlayer* player, const FamTrack* music);
// Returns NULL if no music is queued
FAM_API const FamTrack* FAM_CALL fam_player_get_queued_music(const FamPlayer* player);
FAM_API void FAM_CALL fam_player_pause_music(FamPlayer* player);
FAM_API void FAM_CALL fam_player_resume_music(FamPlayer* player);
FAM_API void FAM_CALL fam_player_stop_music(FamPlayer* player);
// Returns NULL if no music is playing
FAM_API const FamTrack* FAM_CALL fam_player_get_music(const FamPlayer* player);
FAM_API FamPlaybackState FAM_CALL fam_player_get_music_state(const FamPlayer* player);

// Plays on the channel given by fam_track_get_sfx_channel, replacing any sound effect already playing there
FAM_API FamResult FAM_CALL fam_player_play_sfx(FamPlayer* player, const FamTrack* sfx);
// Like fam_player_play_sfx, with the frequency multiplied by pitch: 2.0 is an octave up, 0.5 an octave
// down, 1.0 unchanged. For a shift in cents, pass powf(2.0f, cents / 1200.0f). Returns
// FAM_ERROR_INVALID_ARGUMENT if pitch isn't greater than 0. The track itself is never modified,
// the player shifts period values on their way to the APU, the way a real NES sound driver would:
// - Pulse and triangle periods are rounded and clamped to the range the channel can play.
// - Noise only has 16 periods, so the closest one is used and small shifts may not change it.
// - A shifted sound effect never writes the high period register unless the track does, because
//   that write restarts the note on hardware. If a slide or vibrato would need it, the period
//   stops at the edge of the current range of 256 instead, until the track writes the high register.
// - Like on hardware, shifting a low pulse note further down can get it muted by the sweep unit
//   (period >= $400 with sweep shift 0 and negate off), even if the original wasn't.
FAM_API FamResult FAM_CALL fam_player_play_sfx_pitched(FamPlayer* player, const FamTrack* sfx, float pitch);
// Stops the sound effect on the channel, if any, and hands the channel back to the music
// Does nothing for FAM_CHANNEL_NONE or a channel that can't play sound effects
FAM_API void FAM_CALL fam_player_stop_sfx(FamPlayer* player, FamChannel channel);
FAM_API void FAM_CALL fam_player_stop_all_sfx(FamPlayer* player);
// Returns NULL if no sound effect is playing on the channel, if the channel can't play sound effects,
// or for FAM_CHANNEL_NONE
// Safe to call with any track: fam_player_get_sfx(player, fam_track_get_sfx_channel(track)) == track
FAM_API const FamTrack* FAM_CALL fam_player_get_sfx(const FamPlayer* player, FamChannel channel);

// Stops the track wherever the player references it: as the music, as the queued music, and on any
// sound effect channel. Does nothing if the track isn't in use. Stopping the music this way also
// clears the queue, like fam_player_stop_music. Once this returns, the track's buffer can be freed
FAM_API void FAM_CALL fam_player_stop_track(FamPlayer* player, const FamTrack* track);

#ifdef __cplusplus
}
#endif
