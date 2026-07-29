#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FT_MAGIC "FamiTracker Module"

#define FT_VERSION_NEW 0x0200
#define FT_VERSION_LATEST 0x0440

#define FT_NOTE_COUNT 96
#define FT_MAX_SEQUENCE_LENGTH 253

#define FT_DEFAULT_TEMPO_NTSC 150
#define FT_DEFAULT_TEMPO_PAL 125
#define FT_DEFAULT_SPEED 6
#define FT_DEFAULT_SPEED_SPLIT_POINT 32
#define FT_OLD_SPEED_SPLIT_POINT 21

#define FT_DEFAULT_NAMCO_CHANNELS 1

#define FT_MAX_DPCM_SAMPLES 64
#define FT_OCTAVE_RANGE 8
#define FT_MAX_INSTRUMENTS 64  // A value of 64 in a note's instrument column means "none"
#define FT_MAX_SEQUENCES 128   // Sequence slots per document (indices are sparse within this)
#define FT_MAX_PATTERN 128     // Pattern slots per channel (indices are sparse within this)
#define FT_MAX_TRACKS 64       // Maximum number of tracks (songs) in a document
#define FT_MAX_PATTERN_LENGTH 256 // Maximum rows in a pattern
#define FT_FRAMERATE_NTSC 60      // Engine ticks per second (NTSC nominal, for tempo math)

typedef enum {
    FT_MACHINE_NTSC = 0,
    FT_MACHINE_PAL
} FtMachineType;

typedef enum {
    FT_VIBRATO_OLD = 0,
    FT_VIBRATO_NEW
} FtVibratoType;

typedef enum {
    FT_INSTRUMENT_NONE = 0,
    FT_INSTRUMENT_2A03 = 1,
    FT_INSTRUMENT_VRC6,
    FT_INSTRUMENT_VRC7,
    FT_INSTRUMENT_FDS,
    FT_INSTRUMENT_N163,
    FT_INSTRUMENT_SSB
} FtInstrumentType;

typedef enum {
    FT_EF_NONE = 0,
    FT_EF_SPEED,
    FT_EF_JUMP,
    FT_EF_SKIP,
    FT_EF_HALT,
    FT_EF_VOLUME,
    FT_EF_PORTAMENTO,
    FT_EF_PORTAOFF,				// unused!!
    FT_EF_SWEEPUP,
    FT_EF_SWEEPDOWN,
    FT_EF_ARPEGGIO,
    FT_EF_VIBRATO,
    FT_EF_TREMOLO,
    FT_EF_PITCH,
    FT_EF_DELAY,
    FT_EF_DAC,
    FT_EF_PORTA_UP,
    FT_EF_PORTA_DOWN,
    FT_EF_DUTY_CYCLE,
    FT_EF_SAMPLE_OFFSET,
    FT_EF_SLIDE_UP,
    FT_EF_SLIDE_DOWN,
    FT_EF_VOLUME_SLIDE,
    FT_EF_FT_NOTE_CUT,
    FT_EF_RETRIGGER,
    FT_EF_DELAYED_VOLUME,			// Unimplemented
    FT_EF_FDS_MOD_DEPTH,
    FT_EF_FDS_MOD_SPEED_HI,
    FT_EF_FDS_MOD_SPEED_LO,
    FT_EF_DPCM_PITCH,
    FT_EF_SUNSOFT_ENV_LO,
    FT_EF_SUNSOFT_ENV_HI,
    FT_EF_SUNSOFT_ENV_TYPE,
//	FT_EF_TARGET_VOLUME_SLIDE, 
/*
    FT_EF_VRC7_MODULATOR,
    FT_EF_VRC7_CARRIER,
    FT_EF_VRC7_LEVELS,
*/
} FtEffectType;

typedef enum {
    FT_CHAN_ID_SQUARE1,
    FT_CHAN_ID_SQUARE2,
    FT_CHAN_ID_TRIANGLE,
    FT_CHAN_ID_NOISE,
    FT_CHAN_ID_DPCM,

    FT_CHAN_ID_VRC6_PULSE1,
    FT_CHAN_ID_VRC6_PULSE2,
    FT_CHAN_ID_VRC6_SAWTOOTH,

    FT_CHAN_ID_MMC5_SQUARE1,
    FT_CHAN_ID_MMC5_SQUARE2,
    FT_CHAN_ID_MMC5_VOICE,

    FT_CHAN_ID_N163_CHAN1,
    FT_CHAN_ID_N163_CHAN2,
    FT_CHAN_ID_N163_CHAN3,
    FT_CHAN_ID_N163_CHAN4,
    FT_CHAN_ID_N163_CHAN5,
    FT_CHAN_ID_N163_CHAN6,
    FT_CHAN_ID_N163_CHAN7,
    FT_CHAN_ID_N163_CHAN8,

    FT_CHAN_ID_FDS,

    FT_CHAN_ID_VRC7_CH1,
    FT_CHAN_ID_VRC7_CH2,
    FT_CHAN_ID_VRC7_CH3,
    FT_CHAN_ID_VRC7_CH4,
    FT_CHAN_ID_VRC7_CH5,
    FT_CHAN_ID_VRC7_CH6,

    FT_CHAN_ID_S5B_CH1,
    FT_CHAN_ID_S5B_CH2,
    FT_CHAN_ID_S5B_CH3,

    FT_CHAN_COUNT
} FtChannelId;

typedef enum {
    FT_NOTE_NONE                   = 0,
    FT_NOTE_C,
    FT_NOTE_Cs,
    FT_NOTE_D,
    FT_NOTE_Ds,
    FT_NOTE_E,
    FT_NOTE_F,
    FT_NOTE_Fs,
    FT_NOTE_G,
    FT_NOTE_Gs,
    FT_NOTE_A,
    FT_NOTE_As,
    FT_NOTE_B,
    FT_NOTE_RELEASE,
    FT_NOTE_HALT
} FtNoteType;

typedef enum {
    FT_SOUNDCHIP_NONE               = 0,
    FT_SOUNDCHIP_VRC6               = 1 << 0,
    FT_SOUNDCHIP_VRC7               = 1 << 1,
    FT_SOUNDCHIP_FDS                = 1 << 2,
    FT_SOUNDCHIP_MMC5               = 1 << 3,
    FT_SOUNDCHIP_N163               = 1 << 4,
    FT_SOUNDCHIP_S5B                = 1 << 5
} FtSoundChipFlags;

typedef struct {
    uint8_t index   : 7;
    uint8_t enabled : 1;
} FtSequenceRef;

// Per-note DPCM assignment for a 2A03 instrument (indexed [octave][note])
typedef struct {
    int8_t sample_index;    // -1 = none
    uint8_t pitch : 4;
    uint8_t unused : 3;
    uint8_t loop : 1;
    int8_t  delta;          // initial DAC delta counter, -1 = none
} FtDPCMNote;

// Fat struct of all Famitracker instument data instead of inheritance
typedef struct {
    uint8_t type;
    char name[128];
    union {
        struct {
            FtSequenceRef volume, arpeggio, pitch, hi_pitch, duty_noise;
        };
        FtSequenceRef seq_raw[5];
    };

    FtDPCMNote dpcm_notes[FT_OCTAVE_RANGE][12];
} FtInstrument;

typedef struct {
    uint8_t length;
    uint32_t loop_point;
    uint32_t release_point;
    uint32_t setting;
    int8_t values[FT_MAX_SEQUENCE_LENGTH];
} FtSequenceEff;

typedef struct {
    union {
        struct {
            FtSequenceEff volume, arpeggio, pitch, hi_pitch, duty_noise;
        };
        FtSequenceEff eff_raw[5];
    };
} FtSequenceGroup;


typedef struct {
    char name[256];
    uint32_t length;
    uint32_t offset;    // Byte offset of this sample's raw data within module->sample_data
} FtDPCMSample;

// Field order matches the on-disk layout (note, octave, instrument, volume) so
// block_read_patterns can bulk-read the first four bytes straight into it.
typedef struct {
    uint8_t note;
    uint8_t octave;
    uint8_t instrument;
    uint8_t volume;
    uint8_t eff_number[4];
    uint8_t eff_param[4];
} FtNote;

typedef struct {
    uint32_t speed;
    uint32_t tempo;
    uint32_t channel_count; // Copied from the document so the track is self-contained
    uint8_t* fx_counts; // Array of channel_count additional effect column counts
    uint32_t frame_count;
    uint8_t* frames; // Flat array of frame_count * channel_count pattern indices
    uint32_t pattern_length;
    // Sparse [channel][pattern] map of pointers into pattern_pool (NULL = empty).
    // Indexed as pattern_map[channel * FT_MAX_PATTERN + pattern].
    FtNote** pattern_map;
    // Dense pool holding only the used patterns, each pattern_length notes long.
    FtNote* pattern_pool;
} FtTrack;

struct FamFtModule {
    uint32_t file_version;

    char name[32];
    char artist[32];
    char copyright[32];

    uint32_t global_speed;
    uint32_t global_tempo;
    uint8_t expansion_chip;
    uint32_t channel_count;
    int32_t channels[FT_CHAN_COUNT];
    uint32_t machine;
    uint32_t vibrato_style;
    uint32_t namco_channels;
    uint32_t speed_split_point;

    uint8_t* sample_data;
    FtDPCMSample samples[FT_MAX_DPCM_SAMPLES]; // Fixed slot array; indices are sparse

    uint8_t track_count;
    FtTrack* tracks;

    FtInstrument instruments[FT_MAX_INSTRUMENTS]; // Fixed slot array; indices are sparse

    FtSequenceGroup sequences[FT_MAX_SEQUENCES]; // Fixed slot array; indices are sparse
};

static inline FtNote* track_get_note(
    const FtTrack* track,
    uint32_t pattern_idx,
    uint32_t channel_idx,
    uint32_t row) {

    if (pattern_idx >= FT_MAX_PATTERN ||
        channel_idx >= track->channel_count ||
        row >= track->pattern_length) {
        return NULL;
    }

    FtNote* pattern = track->pattern_map[channel_idx * FT_MAX_PATTERN + pattern_idx];
    if (pattern == NULL) {
        return NULL; // Empty pattern
    }
    return pattern + row;
}
