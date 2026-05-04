#include <stdio.h>
#include <math.h>
#include <SDL3/SDL.h>

#define SAMPLE_RATE 44100
#define FREQUENCY   440.0f

static float phase = 0.0f;

#define CALLBACK_BUFFER_FRAMES 512

static void audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    static float samples[CALLBACK_BUFFER_FRAMES];
    int remaining = additional_amount / sizeof(float);

    while (remaining > 0) {
        int frames = remaining < CALLBACK_BUFFER_FRAMES ? remaining : CALLBACK_BUFFER_FRAMES;
        for (int i = 0; i < frames; i++) {
            samples[i] = sinf(2.0f * SDL_PI_F * phase) * 0.3f;
            phase += FREQUENCY / SAMPLE_RATE;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        SDL_PutAudioStreamData(stream, samples, frames * sizeof(float));
        remaining -= frames;
    }
}

int main(int argc, char **argv) {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        printf("Error initializing SDL: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec spec = {
        .format   = SDL_AUDIO_F32,
        .channels = 1,
        .freq     = SAMPLE_RATE,
    };

    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
    if (!stream) {
        printf("Error opening audio device: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_ResumeAudioStreamDevice(stream);
    printf("Playing 440 Hz sine wave...\n");
    SDL_Delay(3000);

    SDL_DestroyAudioStream(stream);
    SDL_Quit();
    return 0;
}
