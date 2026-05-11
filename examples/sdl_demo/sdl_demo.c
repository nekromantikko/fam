#include <stdio.h>
#include <math.h>
#include <SDL3/SDL.h>
#include <familib.h>

#define SAMPLE_RATE 44100

static void audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    fam_Apu* apu = (fam_Apu*)userdata;
    const double apu_period = 1.0 / fam_apu_get_freq(apu);

    static const double sample_time = 1.0 / (double)SAMPLE_RATE;
    static double accumulator = 0.0;

    int num_samples = additional_amount / sizeof(float);
    for (int i = 0; i < num_samples; i++) {
        static float sample;
        accumulator += sample_time;
        while (accumulator >= apu_period) {
            fam_apu_clock(apu, &sample);
            accumulator -= apu_period;
        }
        SDL_PutAudioStreamData(stream, &sample, sizeof(float));
    }
}

int main(int argc, char **argv) {
    fam_Apu* apu = fam_apu_init();

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        printf("Error initializing SDL: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec spec = {
        .format   = SDL_AUDIO_F32,
        .channels = 1,
        .freq     = SAMPLE_RATE,
    };

    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, apu);
    if (!stream) {
        printf("Error opening audio device: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_ResumeAudioStreamDevice(stream);
    printf("Playing C major arpeggio...\n");

    fam_apu_write_register(apu, FAM_REGISTER_STATUS,              0x05); // enable pulse1 + triangle

    // Pulse 1: 50% duty, loop, constant volume 8, sweep off
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_DUTY,         0xB8);
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_SWEEP,        0x00);

    // Triangle: C3 (t=426=0x1AA, same timer as C4 pulse — triangle runs at 2x clock with 32 steps)
    // loop=1 so length counter won't decay, max linear counter
    fam_apu_write_register(apu, FAM_REGISTER_TRIANGLE_COUNTER,    0xFF);
    fam_apu_write_register(apu, FAM_REGISTER_TRIANGLE_TIMER_LO,   0xAA);
    fam_apu_write_register(apu, FAM_REGISTER_TRIANGLE_TIMER_HI,   0x01);

    // C4 (t=426=0x1AA)
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_TIMER_LO,     0xAA);
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_TIMER_HI,     0x01);
    SDL_Delay(500);

    // E4 (t=338=0x152)
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_TIMER_LO,     0x52);
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_TIMER_HI,     0x01);
    SDL_Delay(500);

    // G4 (t=284=0x11C)
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_TIMER_LO,     0x1C);
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_TIMER_HI,     0x01);
    SDL_Delay(500);

    // C5 (t=213=0xD5)
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_TIMER_LO,     0xD5);
    fam_apu_write_register(apu, FAM_REGISTER_PULSE1_TIMER_HI,     0x00);
    SDL_Delay(500);

    SDL_DestroyAudioStream(stream);
    SDL_Quit();

    fam_apu_free(apu);

    return 0;
}
