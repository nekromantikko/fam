#include <stdio.h>
#include <SDL3/SDL.h>
#include <fam/fam.h>

#define SAMPLE_RATE 44100
#define CMD_BUFFER_CAPACITY 256 // NOTE: Must be power of 2!
#define MAX_CMD_PER_CALLBACK 16
#define CHUNK_SAMPLES 512 // Render buffer, 2 kB of stack

typedef enum {
    CMD_MUSIC_PLAY,
    CMD_MUSIC_PAUSE,
    CMD_MUSIC_RESUME,
    CMD_MUSIC_STOP,
    CMD_SFX_PLAY,
} PlayerCommandType;

typedef struct PlayerCommand {
    PlayerCommandType type;
    const void* ptr;
} PlayerCommand;

// SPSC ring buffer, producer increments tail, consumer increments head
typedef struct CommandBuffer {
    PlayerCommand buffer[CMD_BUFFER_CAPACITY];
    SDL_AtomicU32 head;
    SDL_AtomicU32 tail;
} CommandBuffer;

typedef struct CallbackData {
    FamPlayer* player;
    CommandBuffer* cmd_buffer;
} CallbackData;

static bool cmd_buffer_push(CommandBuffer* cb, PlayerCommandType type, const void* ptr) {
    uint32_t tail = cb->tail.value;
    uint32_t head = SDL_GetAtomicU32(&cb->head);

    if (tail - head >= CMD_BUFFER_CAPACITY) return false; // Full

    const PlayerCommand cmd = {
        .type = type,
        .ptr = ptr
    };
    cb->buffer[tail & (CMD_BUFFER_CAPACITY - 1)] = cmd;
    SDL_SetAtomicU32(&cb->tail, tail + 1);
    return true;
}

static bool cmd_buffer_pop(CommandBuffer* cb, PlayerCommand* out_cmd) {
    uint32_t head = cb->head.value;
    uint32_t tail = SDL_GetAtomicU32(&cb->tail);

    if (head == tail) return false; // Empty

    *out_cmd = cb->buffer[head & (CMD_BUFFER_CAPACITY - 1)];
    SDL_SetAtomicU32(&cb->head, head + 1);
    return true;
}

static void process_player_commands(FamPlayer* player, CommandBuffer* cmd_buffer) {
    PlayerCommand cmd;

    for (int i = 0; i < MAX_CMD_PER_CALLBACK; i++) {
        if (!cmd_buffer_pop(cmd_buffer, &cmd)) {
            break;
        }

        switch(cmd.type) {
            case CMD_MUSIC_PLAY:
                fam_player_play_music(player, (const FamTrack*)cmd.ptr);
                break;
            case CMD_MUSIC_PAUSE:
                fam_player_pause_music(player);
                break;
            case CMD_MUSIC_RESUME:
                fam_player_resume_music(player);
                break;
            case CMD_MUSIC_STOP:
                fam_player_stop_music(player);
                break;
            case CMD_SFX_PLAY:
                fam_player_play_sfx(player, (const FamTrack*)cmd.ptr);
                break;
            default:
                break;
        }
    }
}

static void audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    (void)total_amount;
    CallbackData* callback_data = (CallbackData*)userdata;

    process_player_commands(callback_data->player, callback_data->cmd_buffer);

    float chunk[CHUNK_SAMPLES];
    int samples_remaining = additional_amount / (int)sizeof(float);

    while (samples_remaining > 0) {
        const int count = samples_remaining < CHUNK_SAMPLES ? samples_remaining : CHUNK_SAMPLES;
        fam_player_run(callback_data->player, count, chunk);
        SDL_PutAudioStreamData(stream, chunk, count * (int)sizeof(float));
        samples_remaining -= count;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <song.fam>\n", argv[0]);
        printf("Please provide a .fam music file to play.\n");
        return 1;
    }

    size_t file_size;
    void* file_data = SDL_LoadFile(argv[1], &file_size);
    if (file_data == NULL) {
        printf("Could not read '%s': %s\n", argv[1], SDL_GetError());
        return 1;
    }

    const FamTrack* music;
    FamResult err = fam_track_from_buffer(&music, file_size, file_data);
    if (err != FAM_SUCCESS) {
        printf("Loading '%s' failed with error code %d\n", argv[1], err);
        SDL_free(file_data);
        return 1;
    }
    if (fam_track_get_usage(music) != FAM_TRACK_USAGE_MUSIC) {
        printf("Track '%s' is not a music track\n", argv[1]);
        SDL_free(file_data);
        return 1;
    }

    void* apu_memory = SDL_aligned_alloc(fam_apu_get_memory_alignment(), fam_apu_get_memory_required());
    FamApuConfig apu_config = {
        .region = fam_track_get_region(music),
        .sample_rate = SAMPLE_RATE,
        .format = FAM_AUDIO_F32
    };
    FamApu* apu;
    err = fam_apu_init(&apu, apu_memory, &apu_config);
    if (err != FAM_SUCCESS) {
        printf("Initializing APU failed with error code %d\n", err);
        return 1;
    }

    void* player_memory = SDL_aligned_alloc(fam_player_get_memory_alignment(), fam_player_get_memory_required());
    FamPlayer* player;
    err = fam_player_init(&player, player_memory, apu);
    if (err != FAM_SUCCESS) {
        printf("Initializing player failed with error code %d\n", err);
        return 1;
    }

    CommandBuffer cmd_buffer;
    cmd_buffer.head.value = 0;
    cmd_buffer.tail.value = 0;

    CallbackData callback_data = {
        .player = player,
        .cmd_buffer = &cmd_buffer
    };

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        printf("Error initializing SDL: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec spec = {
        .format   = SDL_AUDIO_F32,
        .channels = 1,
        .freq     = SAMPLE_RATE,
    };

    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, &callback_data);
    if (!stream) {
        printf("Error opening audio device: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_ResumeAudioStreamDevice(stream);
    printf("Playing %s (%s)...\n", argv[1], fam_track_get_region(music) == FAM_REGION_PAL ? "PAL" : "NTSC");
    printf("Press Enter to quit.\n");

    cmd_buffer_push(&cmd_buffer, CMD_MUSIC_PLAY, music);

    getchar();

    cmd_buffer_push(&cmd_buffer, CMD_MUSIC_STOP, NULL);

    // Wait for music to stop before it can be safely freed
    // Of course, if the music hasn't started by this point yet it would give a false positive,
    // so this naive approach is likely not good for a real project
    while (fam_player_get_music(player) != NULL) {
        SDL_Delay(1);
    }
    SDL_free(file_data);

    SDL_Delay(1000);

    SDL_DestroyAudioStream(stream);
    SDL_Quit();

    fam_player_shutdown(player);
    fam_apu_shutdown(apu);

    SDL_aligned_free(player_memory);
    SDL_aligned_free(apu_memory);

    return 0;
}
