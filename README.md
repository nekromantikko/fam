# fam

`fam` is a lightweight, dependency-free, allocation-free NES audio engine library written in C. Designed to be reasonably portable, for use in modern retro-style games and other applications that require authentic NES sound synthesis without the overhead of full NES system emulation. Because `fam` doesn't emulate the NES's CPU, it's not CPU cycle accurate. Other than that, accuracy is a core focus.

## Why?

I started writing `fam` primarily for use in my own retro-style game engine. I wanted authentic NES audio, but the standard format for NES music, `nsf`, is basically just a ROM: playing it requires emulating an entire NES, and I didn't want to do that just to play some music. That's why I decided to spend months of my life writing a custom solution instead.

So what do I mean by "authentic"? I *could* just play rendered audio files, that's probably what most retro-style games do, and it mostly gets the job done. There's a particular effect I wanted to reproduce though: The NES only has five audio channels, and to get the most out of them, they are usually shared between music and sound effects. If a game wants to utilize all the channels for a music track but also play sound effects, it has to silence the music for the channel the sound effect would play on for the duration of the sound effect. This can cause little quirky glitches, like if the music had a long note playing that gets interrupted by the sound effect, that note then has to be re-triggered after the sound effect ends. To accurately reproduce this, I need to fully emulate the NES's audio synthesizer.

## What fam is NOT

`fam` is **not** an audio playback library. It has no concept of an audio thread, device or a command queue, it only emulates the NES APU, drives music and SFX playback through APU register writes and generates samples. This is a deliberate design choice to make it as portable as possible and work for different use cases. There are a lot of other audio playback libraries out there that probably do the job better than what I could write, and this way `fam` doesn't place arbitrary limitations on the architecture around it. You could run it in a web AudioWorklet or a real-time audio callback, or a secret third thing. See `examples/` for reference implementations.

This also means `fam` does no internal locking. A player (and its APU) must only be called from one thread at a time. If you want to control playback from another thread than the one rendering audio, pass commands through a queue. The SDL demo shows one way to do this.

## Memory Model

`fam` leaves memory allocation as the user's responsibility, and never calls malloc. This was one of the core design goals. Every object is initialized in-place into user provided memory, following a `get_memory_required` / `get_memory_alignment` / `init` pattern. The user is responsible for keeping the memory alive for the lifetime of the objects. Feel free to wrap this in whatever memory model you prefer.

## Audio Format

The built-in audio player uses a custom binary track format for playing music and sound effects. A track consists of 4-byte operations, which are mostly plain APU register write instructions with timing. See [`internal/fam/internal/track.h`](internal/fam/internal/track.h) for details. No converters ship yet, see Roadmap.

Tracks are a flattened recording rather than a score: the region (NTSC or PAL) is baked in at export time, and a track can only be played on an APU of the same region.

The track format is versioned independently of the library. Every future version of `fam` will be able to play tracks made for any earlier release.

## Output Format

Samples are output as mono 32-bit floats (`FAM_AUDIO_F32`) or signed 16-bit integers (`FAM_AUDIO_S16`), in native byte order, at the sample rate given in `FamApuConfig`. All processing happens in floating point and is only quantized when a sample is written, so S16 is the F32 signal rounded to the nearest step and clamped, with nothing converted in a separate pass. A sample rate of 0 outputs one sample per APU cycle (~895 kHz NTSC, ~831 kHz PAL), which is mostly useful for testing or your own resampling.

## Emulation Accuracy

The test suite includes implementations of standard APU accuracy tests adapted from Chris "100th_Coin" Siebert's [AccuracyCoin](https://github.com/100thCoin/AccuracyCoin) test suite (Which is largely based on blargg's tests).
**Tests requiring cycle-accurate CPU-to-APU bus timing are left unimplemented.** `fam` doesn't emulate the CPU and works at APU clock granularity, which is enough to produce accurate output.

## Portability

The library itself only depends on the C standard library (`string.h`, `assert.h`, `stdalign.h`), and doesn't require linking libm. Requires C11 to build. The tests and examples fetch their own dependencies (Unity and SDL3) through CMake.

The library currently assumes a **little-endian** architecture. It *should* also work on big-endian architectures, but that hasn't been tested.

## Current Status

### Core APU Channels

 - Pulse 1 & Pulse 2
 - Triangle
 - Noise
 - Delta Modulation Channel (DMC), reads sample bytes via a user-provided callback

### Audio Player (Driver)

An implementation capable of driving a single music track and up to four sound effects simultaneously, one per channel (pulses, triangle or noise). A sound effect takes over its channel for its duration, and when it ends or is stopped, the music is restored on that channel. Playing a sound effect on a channel that already has one playing replaces the old one.

A music track can also be queued to start the moment the current one ends by itself, with no gap. This covers the classic pattern of a jingle interrupting the background music, which then starts again after it:

```c
fam_player_play_music(player, victory_jingle);
fam_player_queue_music(player, overworld_theme);
```

## Building

`fam` uses CMake (3.22+) and any C11 compiler.

```sh
cmake -B build
cmake --build build --config Release
```

| Option               | Default           | Description                               |
|----------------------|-------------------|-------------------------------------------|
| `BUILD_SHARED_LIBS`  | `OFF`             | Build a shared library instead of static  |
| `FAM_BUILD_TESTS`    | `OFF`             | Build the test suite (fetches Unity)      |
| `FAM_BUILD_EXAMPLES` | `OFF`             | Build the examples (fetches SDL3)         |
| `FAM_INSTALL`        | `ON` if top-level | Generate install rules                    |

Run the tests with:

```sh
cmake -B build -DFAM_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

Debug builds of the library get a `d` suffix (e.g. `famd.lib`), so Debug and Release can be installed side by side.

### Using fam in your project

**With FetchContent or as a subdirectory:**

```cmake
include(FetchContent)
FetchContent_Declare(fam
    GIT_REPOSITORY https://github.com/nekromantikko/fam.git
    GIT_TAG v1.0.0
)
FetchContent_MakeAvailable(fam)

target_link_libraries(my_game PRIVATE fam::fam)
```

**As an installed package:**

```sh
cmake --install build --prefix <prefix>
```

```cmake
find_package(fam 1.0 REQUIRED)
target_link_libraries(my_game PRIVATE fam::fam)
```

**Without CMake:** compile `src/*.c` with `include/` and `internal/` on the include path, and `FAM_BUILD` defined. If you're linking against `fam` as a shared library, define `FAM_SHARED` both when building the library and in your own code.

## API Overview

**NOTE: The API is still subject to change before the first official release!**

Everything is available through `#include <fam/fam.h>`. There are three pieces:

- **`FamApu`**: the APU emulator. Takes register writes, produces samples.
- **`FamTrack`**: a read-only handle to track data in your own buffer. A track is either music or a sound effect, see `fam_track_get_usage`.
- **`FamPlayer`**: drives an APU from a music track and up to four sound effects.

### Minimal example

```c
#include <fam/fam.h>

// alloc_aligned(alignment, size) stands in for your allocator of choice

// 1. Load a track. This validates the buffer, nothing is copied
const FamTrack* music;
if (fam_track_from_buffer(&music, size, data) != FAM_SUCCESS) {
    // Not a valid track
}

// 2. Initialize the APU in memory you provide
void* apu_memory = alloc_aligned(fam_apu_get_memory_alignment(), fam_apu_get_memory_required());
FamApuConfig config = {
    .region = fam_track_get_region(music),
    .sample_rate = 48000,
};
FamApu* apu;
fam_apu_init(&apu, apu_memory, &config);

// 3. Initialize a player and attach it to the APU
void* player_memory = alloc_aligned(fam_player_get_memory_alignment(), fam_player_get_memory_required());
FamPlayer* player;
fam_player_init(&player, player_memory, apu);

// 4. Start playback, then render samples whenever your audio code needs them
fam_player_play_music(player, music);

float samples[512];
fam_player_run(player, 512, samples);
```

See [`examples/sdl_demo`](examples/sdl_demo/sdl_demo.c) for a complete program using a real-time audio callback.

### Rules worth knowing

- **Memory:** `fam` never allocates. Every object lives in memory you provide, sized and aligned according to its `get_memory_required` / `get_memory_alignment`. `shutdown` doesn't free anything, the memory is still yours.
- **Lifetimes:** A track buffer must stay alive and unmodified for as long as its `FamTrack` handle is in use, including while it's playing. The player only references a track while it's playing or queued: once the call that stops, replaces or finishes it returns, the buffer is never touched again.
- **Freeing a track while audio is running:** Call `fam_player_stop_track` on the thread that owns the player. It stops the track wherever it's in use (music, queued music, or a sound effect) and leaves everything else alone. Free the buffer only once that call has returned. Don't find out by reading the player from your own thread, that's a data race: have the audio thread acknowledge it instead. The SDL demo does this by numbering its commands and publishing how many it has executed.
- **Errors:** Functions that take data from outside the library (`fam_track_from_buffer`, `fam_apu_init`, `fam_player_init`) validate it and return a `FamResult`. Everything else expects valid handles and only checks them with `assert` in debug builds.
- **Threading:** No internal locking. Only call a player from one thread at a time.
- **Ownership:** Once an APU is attached to a player, don't write to it directly.
- **Region:** Playing a track on an APU of the other region returns `FAM_ERROR_REGION_MISMATCH`.
- **Usage:** Passing a sound effect to `fam_player_play_music`, or music to `fam_player_play_sfx`, returns `FAM_ERROR_USAGE_MISMATCH`. `fam_track_get_sfx_channel` returns `FAM_CHANNEL_NONE` for music, and the player's channel functions accept it.
- **Using the APU on its own:** `FamApu` can also be used without a player, by writing registers with `fam_apu_write_register` and generating samples with `fam_apu_run`. If you use the DMC, provide a sample reader with `fam_apu_set_dmc_reader`.

## Roadmap

### Expansion Audio

 - [ ] Famicom Disk System (FDS)
 - [ ] Namco 163
 - [ ] Nintendo MMC5
 - [ ] Konami VRC6
 - [ ] Konami VRC7
 - [ ] Sunsoft 5B

### Track Format Converters

A decision needs to be made about what source formats to support for conversion and whether the converters should live in the library or be separate tools. Converters for tracker formats require replicating their playback logic, and would be a lot to add to an otherwise minimalist library. A NSF converter requires a whole NES emulator. A VGM converter is much more straightforward, because it's already quite similar to `fam`'s own format.

### Tests

The tests currently only cover APU emulation accuracy. Tests should be written to cover APU initialization, player and track. A set of reference tracks should also be added, to verify that tracks from earlier releases keep playing correctly.

## Credits

 - Chris "100th_Coin" Siebert's [AccuracyCoin](https://github.com/100thCoin/AccuracyCoin), and blargg's APU tests it is based on
 - The [NESdev Wiki](https://www.nesdev.org/wiki/APU), for its extensive APU documentation

## License

MIT, see [LICENSE](LICENSE).
