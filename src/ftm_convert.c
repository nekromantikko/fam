#include <fam/ftm.h>
#include <fam/internal/ftm_types.h>
#include <fam/internal/grow_buffer.h>
#include <fam/internal/stream_types.h>
#include <fam/internal/stream_util.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define VOL_SHIFT 3

typedef enum {
    SEQ_DISABLED = 0,
	SEQ_RUNNING,
	SEQ_END,
	SEQ_HALT
} SequenceState;

// Instrument sequence types. Order matches FtInstrument.seq_raw / FtSequenceGroup.eff_raw.
typedef enum {
    FT_SEQ_VOLUME = 0,
    FT_SEQ_ARPEGGIO,
    FT_SEQ_PITCH,
    FT_SEQ_HIPITCH,
    FT_SEQ_DUTY,
    FT_SEQ_COUNT
} FtSequenceType;

// Runtime state for one instrument sequence running on a channel.
typedef struct {
    SequenceState state;
    uint16_t pointer;         // current position in the sequence
    const FtSequenceEff* seq; // sequence data, or NULL if none
} SequenceRuntime;

// Runtime state for one channel column during conversion.
typedef struct {
    uint32_t channel_id;        // FT_CHAN_ID_* this column maps to
    bool gate;                  // a note is currently sounding
    uint8_t volume;             // note-column volume, 0..15, shifted left by 3 (0-127 range) for finer volume slides
    uint8_t instrument;         // running instrument (FT_MAX_INSTRUMENTS = none)

    uint8_t seq_volume;         // volume value from instrument sequence 

    int32_t base_period;        // period of the current note (before effects)
    int32_t max_period;         // clamp bound for this channel
    int32_t max_volume;         // clamp bound for this channel

    // Duty
    uint8_t duty_period;        // current duty, 0..3
    uint8_t default_duty;       // Vxx

    // Effects (populated in 5d)
    uint32_t vibrato_depth;
    uint32_t vibrato_speed;
    uint32_t vibrato_phase;
    int8_t pitch_offset;        // Pxx
    uint8_t volume_slide;       // Axy
    uint8_t note_cut;           // Sxx

    SequenceRuntime sequences[FT_SEQ_COUNT]; // volume / arpeggio / pitch / hi-pitch / duty
} ChannelRuntime;

// Where a used sample ends up once packed into a track's DPCM banks. The three
// register fields feed the stream operations emitted during playback conversion.
typedef struct {
    uint8_t bank;       // Bank index -> OP_SWITCH_SAMPLE_BANK
    uint8_t addr;       // Start address in 64-byte units -> $4012 (OP_DMC_WRITE2)
    uint8_t length;     // Length in 16-byte units -> $4013 (OP_DMC_WRITE3)
} SamplePlacement;

// Control-flow effects found on a row: Bxx (jump), Cxx (halt), Dxx (skip).
typedef struct {
    bool halt;          // Cxx encountered
    int32_t jump_frame; // Bxx target frame, or -1
    int32_t skip_row;   // Dxx target row (in the next frame), or -1
} RowControl;

typedef struct {
    int reg_shadow[0x14];  // last emitted $4000-$4013 byte; -1 = force the next write
    bool vibrato_mode;     // FT_VIBRATO_NEW vs OLD (module-wide)
    ChannelRuntime channels[FT_CHAN_COUNT];
} PlaybackState;

static const uint32_t FT_NOTE_TABLE_NTSC[FT_NOTE_COUNT] = {
    0xD5B, 0xC9C, 0xBE6, 0xB3B, 0xA9A, 0xA01, 0x972, 0x8EA, 
    0x86A, 0x7F1, 0x77F, 0x713, 0x6AD, 0x64D, 0x5F3, 0x59D, 
    0x54C, 0x500, 0x4B8, 0x474, 0x434, 0x3F8, 0x3BF, 0x389, 
    0x356, 0x326, 0x2F9, 0x2CE, 0x2A6, 0x280, 0x25C, 0x23A, 
    0x21A, 0x1FB, 0x1DF, 0x1C4, 0x1AB, 0x193, 0x17C, 0x167, 
    0x152, 0x13F, 0x12D, 0x11C, 0x10C, 0x0FD, 0x0EF, 0x0E1, 
    0x0D5, 0x0C9, 0x0BD, 0x0B3, 0x0A9, 0x09F, 0x096, 0x08E, 
    0x086, 0x07E, 0x077, 0x070, 0x06A, 0x064, 0x05E, 0x059, 
    0x054, 0x04F, 0x04B, 0x046, 0x042, 0x03F, 0x03B, 0x038, 
    0x034, 0x031, 0x02F, 0x02C, 0x029, 0x027, 0x025, 0x023, 
    0x021, 0x01F, 0x01D, 0x01B, 0x01A, 0x018, 0x017, 0x015, 
    0x014, 0x013, 0x012, 0x011, 0x010, 0x00F, 0x00E, 0x00D, 
};

static const int32_t FT_VIBRATO_TABLE_OLD[256] = {
    0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 
    0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 
    0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 
    0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x002, 0x002, 0x003, 0x003, 0x003, 0x003, 0x003, 
    0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x002, 0x003, 0x003, 0x003, 0x003, 0x004, 0x004, 0x004, 0x004, 
    0x001, 0x001, 0x001, 0x002, 0x002, 0x003, 0x003, 0x004, 0x004, 0x004, 0x005, 0x005, 0x006, 0x006, 0x007, 0x007, 
    0x001, 0x001, 0x002, 0x002, 0x003, 0x003, 0x004, 0x004, 0x005, 0x005, 0x006, 0x006, 0x007, 0x007, 0x008, 0x008, 
    0x001, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x00A, 0x00B, 0x00C, 0x00D, 0x00E, 0x00F, 
    0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x00A, 0x00B, 0x00C, 0x00D, 0x00E, 0x00F, 0x010, 
    0x001, 0x002, 0x004, 0x006, 0x008, 0x00A, 0x00C, 0x00E, 0x010, 0x012, 0x014, 0x016, 0x018, 0x01A, 0x01C, 0x01E, 
    0x001, 0x003, 0x005, 0x007, 0x009, 0x00B, 0x00D, 0x00F, 0x011, 0x013, 0x015, 0x017, 0x019, 0x01B, 0x01D, 0x01F, 
    0x001, 0x004, 0x008, 0x00C, 0x010, 0x014, 0x018, 0x01C, 0x020, 0x024, 0x028, 0x02C, 0x030, 0x034, 0x038, 0x03C, 
    0x001, 0x005, 0x009, 0x00D, 0x011, 0x015, 0x019, 0x01D, 0x021, 0x025, 0x029, 0x02D, 0x031, 0x035, 0x039, 0x03D, 
    0x001, 0x008, 0x010, 0x018, 0x020, 0x028, 0x030, 0x038, 0x040, 0x048, 0x050, 0x058, 0x060, 0x068, 0x070, 0x078, 
    0x001, 0x009, 0x011, 0x019, 0x021, 0x029, 0x031, 0x039, 0x041, 0x049, 0x051, 0x059, 0x061, 0x069, 0x071, 0x079, 
    0x001, 0x010, 0x020, 0x030, 0x040, 0x050, 0x060, 0x070, 0x080, 0x090, 0x0A0, 0x0B0, 0x0C0, 0x0D0, 0x0E0, 0x0F0, 
};

static const int32_t FT_VIBRATO_TABLE_NEW[256] = {
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 
    0x000, 0x000, 0x000, 0x000, 0x000, 0x001, 0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 
    0x000, 0x000, 0x000, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x003, 0x003, 0x003, 0x003, 0x003, 0x003, 0x003, 
    0x000, 0x000, 0x000, 0x001, 0x001, 0x002, 0x002, 0x003, 0x003, 0x003, 0x004, 0x004, 0x004, 0x004, 0x004, 0x004, 
    0x000, 0x000, 0x001, 0x002, 0x002, 0x003, 0x003, 0x004, 0x004, 0x005, 0x005, 0x006, 0x006, 0x006, 0x006, 0x006, 
    0x000, 0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x007, 0x008, 0x008, 0x009, 0x009, 0x009, 0x009, 
    0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x009, 0x00A, 0x00B, 0x00B, 0x00B, 0x00B, 
    0x000, 0x001, 0x002, 0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x00A, 0x00B, 0x00C, 0x00C, 0x00D, 0x00D, 0x00D, 
    0x000, 0x001, 0x003, 0x004, 0x006, 0x008, 0x009, 0x00A, 0x00C, 0x00D, 0x00E, 0x00E, 0x00F, 0x010, 0x010, 0x010, 
    0x000, 0x002, 0x004, 0x006, 0x008, 0x00A, 0x00C, 0x00D, 0x00F, 0x011, 0x012, 0x013, 0x014, 0x015, 0x015, 0x015, 
    0x000, 0x002, 0x005, 0x008, 0x00B, 0x00E, 0x010, 0x013, 0x015, 0x017, 0x018, 0x01A, 0x01B, 0x01C, 0x01D, 0x01D, 
    0x000, 0x004, 0x008, 0x00C, 0x010, 0x014, 0x018, 0x01B, 0x01F, 0x022, 0x024, 0x026, 0x028, 0x02A, 0x02B, 0x02B, 
    0x000, 0x006, 0x00C, 0x012, 0x018, 0x01E, 0x023, 0x028, 0x02D, 0x031, 0x035, 0x038, 0x03B, 0x03D, 0x03E, 0x03F, 
    0x000, 0x009, 0x012, 0x01B, 0x024, 0x02D, 0x035, 0x03C, 0x043, 0x04A, 0x04F, 0x054, 0x058, 0x05B, 0x05E, 0x05F, 
    0x000, 0x00C, 0x018, 0x025, 0x030, 0x03C, 0x047, 0x051, 0x05A, 0x062, 0x06A, 0x070, 0x076, 0x07A, 0x07D, 0x07F, 
};

static inline int32_t max_volume_from_channel_id(int32_t channel_id) {
    switch (channel_id) {
        case FT_CHAN_ID_SQUARE1:
        case FT_CHAN_ID_SQUARE2:
        case FT_CHAN_ID_TRIANGLE:
        case FT_CHAN_ID_NOISE:
        case FT_CHAN_ID_DPCM:
            return 0x0F;
        case FT_CHAN_ID_VRC6_PULSE1:
        case FT_CHAN_ID_VRC6_PULSE2:
        case FT_CHAN_ID_VRC6_SAWTOOTH:
            return 0x0F;
        case FT_CHAN_ID_MMC5_SQUARE1:
        case FT_CHAN_ID_MMC5_SQUARE2:
            return 0x0F;
        case FT_CHAN_ID_N163_CHAN1:
        case FT_CHAN_ID_N163_CHAN2:
        case FT_CHAN_ID_N163_CHAN3:
        case FT_CHAN_ID_N163_CHAN4:
        case FT_CHAN_ID_N163_CHAN5:
        case FT_CHAN_ID_N163_CHAN6:
        case FT_CHAN_ID_N163_CHAN7:
        case FT_CHAN_ID_N163_CHAN8:
            return 0x0F;
        case FT_CHAN_ID_FDS:
            return 0x20;
        case FT_CHAN_ID_VRC7_CH1:
        case FT_CHAN_ID_VRC7_CH2:
        case FT_CHAN_ID_VRC7_CH3:
        case FT_CHAN_ID_VRC7_CH4:
        case FT_CHAN_ID_VRC7_CH5:
        case FT_CHAN_ID_VRC7_CH6:
            return 0x0F;
        case FT_CHAN_ID_S5B_CH1:
        case FT_CHAN_ID_S5B_CH2:
        case FT_CHAN_ID_S5B_CH3:
            return 0x0F;
    }

    return 0;
}

static inline int32_t max_period_from_channel_id(int32_t channel_id) {
    switch (channel_id) {
        case FT_CHAN_ID_SQUARE1:
        case FT_CHAN_ID_SQUARE2:
        case FT_CHAN_ID_TRIANGLE:
        case FT_CHAN_ID_NOISE:
        case FT_CHAN_ID_DPCM:
            return 0x7FF;
        case FT_CHAN_ID_VRC6_PULSE1:
        case FT_CHAN_ID_VRC6_PULSE2:
        case FT_CHAN_ID_VRC6_SAWTOOTH:
            return 0x7FF;
        case FT_CHAN_ID_MMC5_SQUARE1:
        case FT_CHAN_ID_MMC5_SQUARE2:
            return 0xFFF;
        case FT_CHAN_ID_N163_CHAN1:
        case FT_CHAN_ID_N163_CHAN2:
        case FT_CHAN_ID_N163_CHAN3:
        case FT_CHAN_ID_N163_CHAN4:
        case FT_CHAN_ID_N163_CHAN5:
        case FT_CHAN_ID_N163_CHAN6:
        case FT_CHAN_ID_N163_CHAN7:
        case FT_CHAN_ID_N163_CHAN8:
            return 0xFFFF;
        case FT_CHAN_ID_FDS:
            return 0xFFF;
        case FT_CHAN_ID_VRC7_CH1:
        case FT_CHAN_ID_VRC7_CH2:
        case FT_CHAN_ID_VRC7_CH3:
        case FT_CHAN_ID_VRC7_CH4:
        case FT_CHAN_ID_VRC7_CH5:
        case FT_CHAN_ID_VRC7_CH6:
            return 0x7FF;
        case FT_CHAN_ID_S5B_CH1:
        case FT_CHAN_ID_S5B_CH2:
        case FT_CHAN_ID_S5B_CH3:
            return 0xFFF;
    }

    return 0;
}

static inline int32_t channel_get_vibrato(const ChannelRuntime* channel, bool mode) {
    if (channel->vibrato_depth == 0) {
        return 0;
    }

    const int32_t* table = mode == FT_VIBRATO_NEW ? FT_VIBRATO_TABLE_NEW : FT_VIBRATO_TABLE_OLD;

    const int32_t row = (channel->vibrato_depth & 0x0F) << 4;
    const int32_t col = channel->vibrato_phase & 0x0F;

    int32_t freq = 0;
    switch(channel->vibrato_phase >> 4) {
        case 0:                     // Rising
            freq = table[row + col];
            break;
        case 1:                     // Falling
            freq = table[row + 15 - col];
            break;
        case 2:                     // Rising negative
            freq = -table[row + col];
            break;
        case 3:                     // Falling negative
            freq = -table[row + 15 - col];
            break;
    }

    if (mode != FT_VIBRATO_NEW) {
        freq = table[row + 15] + 1;
        freq >>= 1;
    }

    return freq;
}

static inline int32_t clamp_period(int32_t period, int32_t max_period) {
    if (period < 0) {
        period = 0;
    } else if (period > max_period) {
        period = max_period;
    }
    return period;
}

static inline int32_t channel_calculate_period(const ChannelRuntime* channel, bool vibrato_mode) {
    return clamp_period(channel->base_period - channel_get_vibrato(channel, vibrato_mode) + channel->pitch_offset, channel->max_period);
}

static inline int32_t channel_get_tremolo(const ChannelRuntime* channel) {
    // TODO!
    return 0;
}

static inline int32_t clamp_volume(int32_t volume, int32_t max_volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > max_volume) {
        volume = max_volume;
    }
    return volume;
}

static inline int32_t channel_calculate_volume(const ChannelRuntime* channel) {
    if (!channel->gate) {
        return 0;
    }

    int32_t volume = channel->volume >> VOL_SHIFT;
    volume = clamp_volume((volume * channel->seq_volume) / 15 - channel_get_tremolo(channel), channel->max_volume);

    // If both inputs > 0, output at least 1
    if (volume == 0 && channel->seq_volume > 0 && channel->volume > 0) {
        return 1;
    }

    return volume;
}

static uint64_t channel_mask_from_ftm_channel_id(uint32_t channel_id) {
    switch (channel_id) {
        case FT_CHAN_ID_SQUARE1:
            return CHAN_BIT_PULSE1;
        case FT_CHAN_ID_SQUARE2:
            return CHAN_BIT_PULSE2;
        case FT_CHAN_ID_TRIANGLE:
            return CHAN_BIT_TRIANGLE;
        case FT_CHAN_ID_NOISE:
            return CHAN_BIT_NOISE;
        case FT_CHAN_ID_DPCM:
            return CHAN_BIT_DMC;
        default:
            // TODO: Add expansion chip support
            return 0;
    }
}

// NOTE: Returns a fam channel mask
static uint64_t scan_active_channels(const FtTrack* track, uint32_t channel_count, const uint32_t* channels) {
    uint64_t result = 0;
    const uint64_t result_max = (1 << CHAN_COUNT) - 1;

    for (uint32_t frame = 0; frame < track->frame_count && result != result_max; frame++) {
        for (uint32_t row = 0; row < track->pattern_length; row++) {
            for (uint32_t channel = 0; channel < channel_count; channel++) {
                uint32_t channel_id = channels[channel];
                uint64_t mask = channel_mask_from_ftm_channel_id(channel_id);
                if (result & mask) {
                    continue;
                }
                uint8_t pattern = track->frames[frame*channel_count + channel];
                const FtNote* note = track_get_note(track, pattern, channel, row);
                if (note == NULL) {
                    continue; // Frame references an undefined pattern
                }

                // A channel is active if it plays a note or has any effect on it
                // (effects like volume/pitch/jumps still need the channel driven).
                bool active = note->note != FT_NOTE_NONE;
                for (int k = 0; !active && k < 4; k++) {
                    active = note->eff_number[k] != FT_EF_NONE;
                }
                if (active) {
                    result |= mask;
                }
            }
        }
    }

    return result;
}

// Marks which document samples this track triggers. Mirrors FamiTracker's
// ScanSong: walks the DPCM channel in play order carrying a running instrument,
// then resolves each real note through that instrument's per-note DPCM map.
// Unlike FamiTracker we only carry the running instrument across rows that
// actually play a note, which keeps us correct against empty (zero-filled) rows
// where fam can't distinguish "instrument 0" from "no instrument column".
static void scan_used_samples(const FamFtModule* module, const FtTrack* track, bool* used) {
    int dpcm_channel = -1;
    for (uint32_t c = 0; c < module->channel_count; c++) {
        if (module->channels[c] == FT_CHAN_ID_DPCM) {
            dpcm_channel = (int)c;
            break;
        }
    }
    if (dpcm_channel < 0) {
        return; // Document has no DPCM channel
    }

    uint8_t instrument = 0; // FamiTracker's initial running instrument
    for (uint32_t frame = 0; frame < track->frame_count; frame++) {
        uint8_t pattern = track->frames[frame * module->channel_count + dpcm_channel];
        for (uint32_t row = 0; row < track->pattern_length; row++) {
            const FtNote* note = track_get_note(track, pattern, dpcm_channel, row);
            if (note == NULL || note->note < FT_NOTE_C || note->note > FT_NOTE_B) {
                continue; // Only real notes (not empty/release/halt) trigger a sample
            }

            if (note->instrument < FT_MAX_INSTRUMENTS) {
                instrument = note->instrument;
            }

            // Undefined instrument slots are zeroed (type FT_INSTRUMENT_NONE),
            // so the type check below filters them; the index is always in range
            // since the running instrument only takes values < FT_MAX_INSTRUMENTS.
            const FtInstrument* inst = &module->instruments[instrument];
            if (inst->type != FT_INSTRUMENT_2A03 || note->octave >= FT_OCTAVE_RANGE) {
                continue;
            }

            uint8_t sample_index = inst->dpcm_notes[note->octave][note->note - FT_NOTE_C].sample_index;
            if (sample_index > 0 && sample_index <= FT_MAX_DPCM_SAMPLES) {
                used[sample_index - 1] = true;
            }
        }
    }
}

// Computes the DPCM bank layout for the track's used samples. Fills placements[] with each
// sample's bank/address/length register values and bank_sizes[] (room for MAX_DPCM_BANK_COUNT)
// with each bank's byte size, and returns the bank count via *out_bank_count. Next-fit:
// samples are laid out 64-byte aligned within a bank, and a sample that would overflow the
// 16 KB window starts a fresh bank (never split). No allocation — see fill_sample_banks.
static FamResult compute_sample_banks(
    const FamFtModule* module,
    const bool* used,
    SamplePlacement* placements,
    uint32_t* bank_sizes,
    uint32_t* out_bank_count) {

    uint32_t bank_index = 0;
    uint32_t bank_offset = 0; // Byte offset within the current bank
    uint32_t bank_count = 0;

    // Samples with length 0 (undefined slots, or none defined at all) are skipped, so no
    // explicit "no sample block" guard is needed.
    for (int s = 0; s < FT_MAX_DPCM_SAMPLES; s++) {
        if (!used[s] || module->samples[s].length == 0) {
            continue; // Skip unused and empty samples (matches FamiTracker)
        }

        uint32_t size = module->samples[s].length;
        if (bank_offset + size > MAX_DPCM_SAMPLE_BANK_SIZE) {
            bank_index++;
            bank_offset = 0;
        }
        if (bank_index >= MAX_DPCM_BANK_COUNT) {
            return FAM_ERROR_UNSUPPORTED_FEATURE; // Too many samples to fit in banks
        }

        placements[s].bank = (uint8_t)bank_index;
        placements[s].addr = (uint8_t)(bank_offset >> 6);
        placements[s].length = (uint8_t)(size >> 4);

        bank_sizes[bank_index] = bank_offset + size; // Bank need only span its last sample
        bank_count = bank_index + 1;

        // Advance to the next 64-byte boundary for the following sample
        uint32_t adjust = (0x40 - ((bank_offset + size) & 0x3F)) & 0x3F;
        bank_offset += size + adjust;
    }

    *out_bank_count = bank_count;
    return FAM_SUCCESS;
}

// Copies each used sample's raw bytes into its assigned slot within the already-allocated
// banks (bank->data pointers must be set, e.g. by fam_music_alloc).
static void fill_sample_banks(
    const FamFtModule* module,
    const bool* used,
    const SamplePlacement* placements,
    DPCMSampleBank* banks) {

    for (int s = 0; s < FT_MAX_DPCM_SAMPLES; s++) {
        if (!used[s] || module->samples[s].length == 0) {
            continue;
        }
        uint32_t dst_offset = (uint32_t)placements[s].addr << 6;
        memcpy(banks[placements[s].bank].data + dst_offset,
               module->sample_data + module->samples[s].offset,
               module->samples[s].length);
    }
}

// Emits one StreamOperation (opcode + data byte) into the growable stream buffer.
static FamResult stream_emit(GrowBuffer* buffer, uint8_t opcode, uint8_t data) {
    uint8_t bytes[2] = { opcode, data };
    return grow_buffer_write_bytes(buffer, bytes, sizeof(bytes));
}

static RowControl scan_row_control(const FtTrack* track, uint32_t frame, uint32_t row) {
    RowControl ctrl = { false, -1, -1 };
    for (uint32_t ch = 0; ch < track->channel_count; ch++) {
        uint8_t pattern = track->frames[frame * track->channel_count + ch];
        const FtNote* note = track_get_note(track, pattern, ch, row);
        if (note == NULL) {
            continue;
        }
        // Unused effect columns are zeroed (FT_EF_NONE), so scanning all 4 is safe.
        // Later columns/channels overwrite earlier ones, matching FamiTracker.
        for (int k = 0; k < 4; k++) {
            switch (note->eff_number[k]) {
                case FT_EF_JUMP: ctrl.jump_frame = note->eff_param[k]; break;
                case FT_EF_SKIP: ctrl.skip_row = note->eff_param[k]; break;
                case FT_EF_HALT: ctrl.halt = true; break;
                default: break;
            }
        }
    }
    return ctrl;
}

// Period (11-bit timer value) for a note from the NTSC table. note is FT_NOTE_C..B.
static int32_t note_to_period(uint8_t note, uint8_t octave) {
    int index = (int)octave * 12 + ((int)note - FT_NOTE_C);
    if (index < 0 || index >= FT_NOTE_COUNT) {
        return 0;
    }
    return (int32_t)FT_NOTE_TABLE_NTSC[index];
}

// Emits an APU register write only if the value changed since the last emission. The stream
// opcode for a $40xx write is simply (reg - 0x4000).
static FamResult emit_reg(PlaybackState* sim, GrowBuffer* buffer, int reg, uint8_t value) {
    int idx = reg - 0x4000;
    if (sim->reg_shadow[idx] == (int)value) {
        return FAM_SUCCESS;
    }
    sim->reg_shadow[idx] = value;
    return stream_emit(buffer, (uint8_t)idx, value);
}

// Forces the next write to `reg` to be emitted regardless of value. Used on gate-off so the
// next note re-triggers, mirroring FamiTracker resetting m_iLastPeriod to 0xFFFF.
static void force_reg(PlaybackState* sim, int reg) {
    sim->reg_shadow[reg - 0x4000] = -1;
}

// Sets up a channel's instrument sequences on a note trigger. Enabled, non-empty sequences
// start running; the rest are disabled. seq_volume resets to full so a note with no volume
// envelope plays at its column volume.
static void setup_sequences(ChannelRuntime* ch, const FamFtModule* module) {
    ch->seq_volume = 15;

    const FtInstrument* inst = (ch->instrument < FT_MAX_INSTRUMENTS)
        ? &module->instruments[ch->instrument] : NULL;

    for (int t = 0; t < FT_SEQ_COUNT; t++) {
        SequenceRuntime* sr = &ch->sequences[t];
        const FtSequenceEff* seq = NULL;
        if (inst != NULL && inst->type == FT_INSTRUMENT_2A03 && inst->seq_raw[t].enabled) {
            const FtSequenceEff* candidate = &module->sequences[inst->seq_raw[t].index].eff_raw[t];
            if (candidate->length > 0) {
                seq = candidate;
            }
        }
        sr->seq = seq;
        sr->pointer = 0;
        sr->state = (seq != NULL) ? SEQ_RUNNING : SEQ_DISABLED;
    }
}

// Advances one running sequence by a tick, applying its current value to the channel.
// Mirrors FamiTracker's CSequenceHandler::UpdateSequenceRunning.
static void update_sequence_running(ChannelRuntime* ch, int type) {
    SequenceRuntime* sr = &ch->sequences[type];
    const FtSequenceEff* seq = sr->seq;
    int8_t value = seq->values[sr->pointer];

    switch (type) {
        case FT_SEQ_VOLUME: ch->seq_volume = (uint8_t)value; break;
        case FT_SEQ_DUTY:   ch->duty_period = (uint8_t)value; break;
        // TODO(5c-2): FT_SEQ_ARPEGGIO / FT_SEQ_PITCH / FT_SEQ_HIPITCH modify the period.
        default: break;
    }

    sr->pointer++;

    const uint32_t items = seq->length;
    const uint32_t loop = seq->loop_point;
    const uint32_t release = seq->release_point;
    const bool releasing = false; // TODO(5c-2): release-phase sequences

    // (release + 1) wraps to 0 when release == UINT32_MAX (no release); pointer is >= 1 here,
    // so that comparison is simply never true, matching FamiTracker's Release == -1 case.
    if (sr->pointer == (uint32_t)(release + 1) || sr->pointer >= items) {
        if (loop != UINT32_MAX && !(releasing && release != UINT32_MAX)) {
            sr->pointer = (uint16_t)loop;
        } else if (sr->pointer >= items) {
            sr->state = SEQ_END;
        } else if (!releasing) {
            sr->pointer--; // hold at the release point until released
        }
    }
}

// Runs one instrument sequence for a tick based on its state.
static void run_sequence(ChannelRuntime* ch, int type) {
    SequenceRuntime* sr = &ch->sequences[type];
    if (sr->seq == NULL || sr->seq->length == 0 || !ch->gate) {
        return;
    }
    switch (sr->state) {
        case SEQ_RUNNING:
            update_sequence_running(ch, type);
            break;
        case SEQ_END:
            // TODO(5c-2): a fixed arpeggio re-triggers its period here before halting.
            sr->state = SEQ_HALT;
            break;
        default: // SEQ_DISABLED, SEQ_HALT
            break;
    }
}

// Advances all of a channel's instrument sequences by one tick.
static void run_sequences(ChannelRuntime* ch) {
    for (int t = 0; t < FT_SEQ_COUNT; t++) {
        run_sequence(ch, t);
    }
}

// Applies a row's note data to a channel's running state: note trigger (which restarts the
// instrument's sequences), volume column, instrument, and the Vxx duty effect.
static void apply_channel_row(ChannelRuntime* ch, const FtNote* note, const FamFtModule* module) {
    if (note == NULL) {
        return;
    }
    if (note->volume <= 0x0F) { // 0x10 (MAX_VOLUME) means "no change"
        ch->volume = note->volume << VOL_SHIFT;
    }
    if (note->instrument < FT_MAX_INSTRUMENTS) {
        ch->instrument = note->instrument;
    }
    for (int k = 0; k < 4; k++) {
        if (note->eff_number[k] == FT_EF_DUTY_CYCLE) {
            ch->duty_period = note->eff_param[k] & 0x03;
        }
    }
    if (note->note >= FT_NOTE_C && note->note <= FT_NOTE_B) {
        ch->base_period = note_to_period(note->note, note->octave);
        ch->gate = true;
        setup_sequences(ch, module);
    } else if (note->note == FT_NOTE_HALT || note->note == FT_NOTE_RELEASE) {
        ch->gate = false; // TODO(5c-2): release should run the sequence's release phase
    }
}

// Emits a pulse channel's registers for this tick (diff-based). Register layout mirrors
// FamiTracker's CSquareChan::RefreshChannel.
static FamResult tick_pulse(PlaybackState* sim, ChannelRuntime* ch, GrowBuffer* buffer) {
    const int reg = (ch->channel_id == FT_CHAN_ID_SQUARE1) ? 0x4000 : 0x4004;
    int32_t period = channel_calculate_period(ch, sim->vibrato_mode); // clamps to max_period
    int32_t volume = channel_calculate_volume(ch);

    if (volume <= 0) {
        // Silent: mute via $4000, and force $4003 on the next note so it re-triggers.
        force_reg(sim, reg + 3);
        return emit_reg(sim, buffer, reg + 0, 0x30);
    }

    FamResult r;
    if ((r = emit_reg(sim, buffer, reg + 0, (uint8_t)(((ch->duty_period & 0x03) << 6) | 0x30 | volume))) != FAM_SUCCESS) return r;
    if ((r = emit_reg(sim, buffer, reg + 1, 0x08)) != FAM_SUCCESS) return r;                    // disable sweep
    if ((r = emit_reg(sim, buffer, reg + 2, (uint8_t)(period & 0xFF))) != FAM_SUCCESS) return r; // period low
    if ((r = emit_reg(sim, buffer, reg + 3, (uint8_t)((period >> 8) & 0x07))) != FAM_SUCCESS) return r; // period high
    return FAM_SUCCESS;
}

// Ticks one channel, emitting its register writes. Only pulse channels so far
// (triangle/noise = 5e, DPCM = 5f).
static FamResult tick_channel(PlaybackState* sim, ChannelRuntime* ch, GrowBuffer* buffer) {
    run_sequences(ch); // advance instrument envelopes (volume/duty; arp/pitch in 5c-2)
    switch (ch->channel_id) {
        case FT_CHAN_ID_SQUARE1:
        case FT_CHAN_ID_SQUARE2:
            return tick_pulse(sim, ch, buffer);
        default:
            return FAM_SUCCESS; // TODO(5e/5f)
    }
}

// Simulates the track's playback tick by tick, emitting stream operations into `buffer`.
// Speed/tempo fold into the number of engine-tick frames per row via FamiTracker's tempo
// accumulator. Stops when a Cxx halt is hit or playback returns to a position it has
// already emitted (the loop). Sets *out_loop_point to the stream-op index to loop back to,
// or MUSIC_NO_LOOP on halt.
static FamResult simulate_track(const FamFtModule* module, const FtTrack* track, GrowBuffer* buffer, uint32_t* out_loop_point) {
    const uint32_t frames = track->frame_count;
    const uint32_t rows = track->pattern_length;
    const uint32_t speed = track->speed;
    const uint32_t tempo = track->tempo;

    *out_loop_point = MUSIC_NO_LOOP;

    if (frames == 0 || rows == 0) {
        return stream_emit(buffer, OP_ENDSTREAM, 0); // Empty track
    }
    if (speed == 0) {
        return FAM_ERROR_INVALID_FORMAT; // Would divide by zero
    }

    int32_t tempo_decrement = (int32_t)((tempo * 24) / speed);
    if (tempo_decrement < 1) {
        tempo_decrement = 1; // Guard against a stalled accumulator on degenerate tempo
    }
    const int32_t tempo_remainder = (int32_t)((tempo * 24) % speed);
    int32_t tempo_accum = 0;

    // visited[frame*rows + row] = (stream-op index + 1); 0 = not yet visited.
    uint32_t* visited = (uint32_t*)calloc((size_t)frames * rows, sizeof(uint32_t));
    if (visited == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    PlaybackState sim;
    for (int i = 0; i < 0x14; i++) {
        sim.reg_shadow[i] = -1;
    }
    sim.vibrato_mode = (module->vibrato_style == FT_VIBRATO_NEW);
    for (uint32_t col = 0; col < track->channel_count; col++) {
        int32_t channel_id = module->channels[col];
        sim.channels[col] = (ChannelRuntime){
            .channel_id = (uint32_t)channel_id,
            .gate = false,
            .volume = 15 << VOL_SHIFT,
            .seq_volume = 15,
            .instrument = FT_MAX_INSTRUMENTS,
            .max_period = max_period_from_channel_id(channel_id),
            .max_volume = max_volume_from_channel_id(channel_id),
        };
    }

    FamResult result = FAM_SUCCESS;
    uint32_t play_frame = 0;
    uint32_t play_row = 0;

    while (true) {
        const bool row_boundary = (tempo_accum <= 0);
        RowControl ctrl = { false, -1, -1 };
        uint32_t next_frame = play_frame;
        uint32_t next_row = play_row;

        if (row_boundary) {
            const size_t vis = (size_t)play_frame * rows + play_row;
            const uint32_t op_index = (uint32_t)(buffer->size / sizeof(StreamOperation));

            if (visited[vis] != 0) {
                *out_loop_point = visited[vis] - 1; // Playback returns here: this is the loop
                break;
            }
            visited[vis] = op_index + 1;

            ctrl = scan_row_control(track, play_frame, play_row);

            if (ctrl.jump_frame >= 0) {
                next_frame = ((uint32_t)ctrl.jump_frame < frames) ? (uint32_t)ctrl.jump_frame : 0;
                next_row = 0;
            } else if (ctrl.skip_row >= 0) {
                next_frame = (play_frame + 1 < frames) ? play_frame + 1 : 0;
                next_row = ((uint32_t)ctrl.skip_row < rows) ? (uint32_t)ctrl.skip_row : 0;
            } else {
                next_row = play_row + 1;
                next_frame = play_frame;
                if (next_row >= rows) {
                    next_row = 0;
                    next_frame = (play_frame + 1 < frames) ? play_frame + 1 : 0;
                }
            }

            // Apply the row's note data to each channel's running state.
            for (uint32_t col = 0; col < track->channel_count; col++) {
                uint8_t pattern = track->frames[play_frame * track->channel_count + col];
                apply_channel_row(&sim.channels[col], track_get_note(track, pattern, col, play_row), module);
            }
        }

        // Tick each channel and emit its changed register writes.
        for (uint32_t col = 0; col < track->channel_count; col++) {
            // TODO: Effects + Sequences (CChannelHandler::ProcessChannel)



            result = tick_channel(&sim, &sim.channels[col], buffer);
            if (result != FAM_SUCCESS) {
                break;
            }
        }
        if (result != FAM_SUCCESS) {
            break;
        }

        result = stream_emit(buffer, OP_ENDFRAME, 0);
        if (result != FAM_SUCCESS) {
            break;
        }

        if (row_boundary) {
            if (ctrl.halt) {
                *out_loop_point = MUSIC_NO_LOOP;
                break;
            }
            play_frame = next_frame;
            play_row = next_row;
            tempo_accum += (60 * FT_FRAMERATE_NTSC) - tempo_remainder;
        }
        tempo_accum -= tempo_decrement;
    }

    free(visited);

    if (result == FAM_SUCCESS) {
        result = stream_emit(buffer, OP_ENDSTREAM, 0);
    }
    return result;
}

FamResult fam_music_from_ftmodule_track(FamMusic** out_music, const FamFtModule* module, size_t track_idx) {
    if (out_music == NULL || module == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    if (track_idx >= module->track_count) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    const FtTrack* track = &module->tracks[track_idx];

    const uint64_t channel_mask = scan_active_channels(track, module->channel_count, module->channels);
    printf("Track #%zu has channel mask %llu\n", track_idx, channel_mask);

    bool used[FT_MAX_DPCM_SAMPLES] = {0};
    scan_used_samples(module, track, used);

    SamplePlacement placements[FT_MAX_DPCM_SAMPLES] = {0};
    uint32_t bank_sizes[MAX_DPCM_BANK_COUNT] = {0};
    uint32_t bank_count = 0;
    FamResult result = compute_sample_banks(module, used, placements, bank_sizes, &bank_count);
    if (result != FAM_SUCCESS) {
        return result;
    }
    printf("Track #%zu uses %d DPCM sample bank(s)\n", track_idx, bank_count);

    // Simulate playback into a stream buffer and detect the loop point.
    GrowBuffer stream;
    result = grow_buffer_init(&stream, 0x1000);
    if (result != FAM_SUCCESS) {
        return result;
    }
    uint32_t loop_point = MUSIC_NO_LOOP;
    result = simulate_track(module, track, &stream, &loop_point);
    if (result != FAM_SUCCESS) {
        grow_buffer_free(&stream);
        return result;
    }
    const uint32_t op_count = (uint32_t)(stream.size / sizeof(StreamOperation));
    printf("Track #%zu: %u stream ops, loop point %u\n", track_idx, op_count, loop_point);

    // Finalize into a single contiguous FamMusic block (struct + banks + bank data + stream),
    // matching fam_music_from_buffer's layout so fam_music_free works uniformly.
    FamMusic* music; 
    result = music_init(&music, channel_mask, loop_point, bank_count, bank_sizes, op_count);
    if (result != FAM_SUCCESS) {
        grow_buffer_free(&stream);
        return result;
    }
    memcpy(music->stream, stream.data, stream.size);
    grow_buffer_free(&stream);

    fill_sample_banks(module, used, placements, music->dpcm_sample_banks);

    *out_music = music;
    return FAM_SUCCESS;
}
