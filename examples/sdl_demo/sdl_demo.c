#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <SDL3/SDL.h>
#include <fam/fam.h>

#define SAMPLE_RATE 44100
#define CMD_BUFFER_CAPACITY 256 // NOTE: Must be power of 2!
#define MAX_CMD_PER_CALLBACK 16

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
                fam_player_play_music(player, (FamMusic*)cmd.ptr);
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
                fam_player_play_sfx(player, (FamSfx*)cmd.ptr);
                break;
            default:
                break;
        }
    }
}

static void audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    CallbackData* callback_data = (CallbackData*)userdata;

    process_player_commands(callback_data->player, callback_data->cmd_buffer);

    int num_samples = additional_amount / sizeof(float);
    for (int i = 0; i < num_samples; i++) {
        static float sample;
        fam_player_process_samples(callback_data->player, 1, &sample);
        SDL_PutAudioStreamData(stream, &sample, sizeof(float));
    }
}

static bool read_entire_file(const char* path, size_t* out_size, uint8_t** out_data) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (length < 0) {
        fclose(file);
        return false;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)length);
    if (data == NULL) {
        fclose(file);
        return false;
    }

    size_t read_len = fread(data, 1, (size_t)length, file);
    fclose(file);

    if (read_len != (size_t)length) {
        free(data);
        return false;
    }

    *out_size = (size_t)length;
    *out_data = data;
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <song.fam>\n", argv[0]);
        printf("Please provide a .fam music file to play.\n");
        return 1;
    }

    FamApu* apu;
    FamResult err = fam_apu_init(&apu);
    if (err != FAM_SUCCESS) {
        printf("Initializing APU failed with error code %d\n", err);
        return 1;
    }

    FamPlayer* player;
    err = fam_player_init(&player, apu, SAMPLE_RATE);
    if (err != FAM_SUCCESS) {
        printf("Initializing player failed with error code %d\n", err);
        return 1;
    }

    size_t file_size;
    uint8_t* file_data;
    if (!read_entire_file(argv[1], &file_size, &file_data)) {
        printf("Could not read '%s'\n", argv[1]);
        return 1;
    }

    FamMusic* music;
    err = fam_music_from_buffer(&music, file_size, file_data);
    free(file_data);
    if (err != FAM_SUCCESS) {
        printf("Loading '%s' failed with error code %d\n", argv[1], err);
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
    printf("Playing %s...\n", argv[1]);
    printf("Press Enter to quit.\n");

    cmd_buffer_push(&cmd_buffer, CMD_MUSIC_PLAY, music);

    getchar();

    SDL_DestroyAudioStream(stream);
    SDL_Quit();

    fam_music_free(music);
    fam_player_free(player);
    fam_apu_free(apu);

    return 0;
}
