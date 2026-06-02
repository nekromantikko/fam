#include <stdio.h>
#include <math.h>
#include <SDL3/SDL.h>
#include <fam/player.h>

#define SAMPLE_RATE 44100

static const uint8_t arpeggio_data[] = {
    // Header (40 bytes)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Sample data size
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Sample data offset
    0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Record count
    0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Record offset
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Loop point (No loop)

    // Records (51 bytes)
    // Frame 0: channel setup + C4
    0x00, 0x40, 0xB8,  // $4000 PULSE1_DUTY = 0xB8 (50% duty, loop, const vol 8)
    0x01, 0x40, 0x00,  // $4001 PULSE1_SWEEP = 0x00 (sweep off)
    0x08, 0x40, 0xFF,  // $4008 TRI_COUNTER = 0xFF (loop, max linear)
    0x0A, 0x40, 0xAA,  // $400A TRI_TIMER_LO = 0xAA (C3, t=0x1AA)
    0x0B, 0x40, 0x01,  // $400B TRI_TIMER_HI = 0x01
    0x02, 0x40, 0xAA,  // $4002 PULSE1_TIMER_LO = 0xAA (C4, t=0x1AA)
    0x03, 0x40, 0x01,  // $4003 PULSE1_TIMER_HI = 0x01 (note-on)
    0x00, 0x00, 0x1D,  // ENDFRAME, skip 29 (hold for 30 frames total)

    // Frame 30: E4
    0x02, 0x40, 0x52,  // $4002 PULSE1_TIMER_LO = 0x52 (E4, t=0x152)
    0x03, 0x40, 0x01,  // $4003 PULSE1_TIMER_HI = 0x01 (note-on)
    0x00, 0x00, 0x1D,  // ENDFRAME, skip 29

    // Frame 60: G4
    0x02, 0x40, 0x1C,  // $4002 PULSE1_TIMER_LO = 0x1C (G4, t=0x11C)
    0x03, 0x40, 0x01,  // $4003 PULSE1_TIMER_HI = 0x01 (note-on)
    0x00, 0x00, 0x1D,  // ENDFRAME, skip 29

    // Frame 90: C5
    0x02, 0x40, 0xD5,  // $4002 PULSE1_TIMER_LO = 0xD5 (C5, t=0x0D5)
    0x03, 0x40, 0x00,  // $4003 PULSE1_TIMER_HI = 0x00 (note-on)
    0x00, 0x00, 0x1D,  // ENDFRAME, skip 29
};

static void audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    FamPlayer* player = (FamPlayer*)userdata;

    int num_samples = additional_amount / sizeof(float);
    for (int i = 0; i < num_samples; i++) {
        static float sample;
        fam_player_process_samples(player, 1, &sample);
        SDL_PutAudioStreamData(stream, &sample, sizeof(float));
    }
}

int main(int argc, char **argv) {
    FamPlayer* player;
    FamResult err = fam_player_init(SAMPLE_RATE, &player);
    if (err != FAM_SUCCESS) {
        printf("Initializing player failed with error code %d\n", err);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        printf("Error initializing SDL: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec spec = {
        .format   = SDL_AUDIO_F32,
        .channels = 1,
        .freq     = SAMPLE_RATE,
    };

    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, player);
    if (!stream) {
        printf("Error opening audio device: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    fam_player_play_music(player, (FamMusic*)arpeggio_data);

    SDL_ResumeAudioStreamDevice(stream);
    printf("Playing C major arpeggio...\n");

    SDL_Delay(3000);

    SDL_DestroyAudioStream(stream);
    SDL_Quit();

    fam_player_free(player);

    return 0;
}
