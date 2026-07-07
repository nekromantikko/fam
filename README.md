# fam

`fam` is a lightweight, dependency-free NES audio engine library written in C. Designed to be reasonably portable, for use in modern retro-style games and other applications that require authentic NES sound synthesis without the overhead of full NES system emulation.

## Why?

I started writing `fam` primarily for use in my own retro-style game engine. I wanted authentic NES audio but didn't want to emulate an entire NES just to play music. 
`fam` does not natively play standard `.nsf` files. Instead, it uses its own internal data format for sounds and music that bypasses the need for NES CPU emulation.

### The Intended Workflow

Rather than executing raw NES machine code at runtime, the ideal workflow is to write music in modern trackers and load them in `fam`:

- Native Loaders: Planned loaders for FamiTracker (`.ftm`) and FamiStudio (`.fms`) files.
- NSF Conversion: While an NSF loader might be added in the future, it would be inherently lossy. The recommended approach will be converting NSFs to a structured tracker format, cleaning them up, and loading them in `fam`.
- Bindings: Future plans include C# bindings for a Unity plugin.

## What fam is NOT

`fam` is **not** an audio playback library. It has no concept of an audio thread, device or a command queue, it only emulates the NES APU, drives music and SFX playback through APU register writes and generates samples. This is a deliberate design choice to make it as portable as possible and work for different use cases. There are a lot of other audio playback libraries out there that probably do the job better than what I could write, and this way `fam` doesn't place arbitrary limitations for the architecture around it. You could run it in a web AudioWorklet or a real-time audio callback, or a secret third thing. See `examples/` for reference implementations.

## Emulation Accuracy

The test suite includes implementations of standard APU accuracy tests adapted from Chris "100th_Coin" Siebert's [AccuracyCoin](https://github.com/100thCoin/AccuracyCoin) test suite (Which is largely based on blargg's tests).
**Tests requiring cycle-accurate CPU-to-APU bus timing are left unimplemented.** Because the library processes structured audio data rather than raw NES machine code, full hardware cycle-accuracy is neither necessary nor applicable to achieve perfect, authentic sound playback.

## Portability

The library currently assumes a **little-endian** architecture. Big-endian architectures are not officially supported or tested.

## Current Status

### Core APU Channels

 - Pulse 1 & Pulse 2
 - Triangle
 - Noise
 - Delta Modulation Channel (DMC), reads sample bytes via a user-provided callback

### Audio Player (Driver)
 
 An implementation capable of driving a single music track, with four slots for sound effects for the pulse, triangle and noise channels with proper prioritization.

## API Overview

**TODO**

## Roadmap

### APU

- [ ] PAL support

### Audio Player

 - [x] Sound effect support with proper prioritization

### Audio Expansion Chips (Mappers

 - [ ] Famicom Disk System (FDS)
 - [ ] Namco 163
 - [ ] Nintendo MMC5
 - [ ] Konami VRC6
 - [ ] Konami VRC7
 - [ ] Sunsoft 5B
  
### Source Formats
 - [ ] FamiTracker (`.ftm`) native parser
 - [ ] FamiStudio (`.fms`) native parser
