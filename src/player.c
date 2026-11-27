#include <fam/player.h>
#include <fam/apu.h>
#include <fam/track.h>
#include <fam/internal/track.h>
#include <fam/internal/apu_tables.h>
#include <assert.h>
#include <string.h>
#include <stdalign.h>

typedef struct MusicState {
    uint64_t channel_mask;
    uint16_t dpcm_bank_count;

    const uint8_t* data_begin;
    const uint8_t* data_end;
    const uint8_t* data_pos;
    const uint8_t* data_loop;
    uint32_t cycles_remaining;

    const uint8_t* dpcm_bank_data_begin;
    const uint8_t* current_dpcm_bank_data;
    
    bool paused;
} MusicState;

typedef struct SfxState {
    const uint8_t* data_begin;
    const uint8_t* data_end;
    const uint8_t* data_pos;
    uint32_t cycles_remaining;

    bool enabled; // Affects APU channel status but not channel ownership

    // Pitch shift, see player_write_sfx_period. Period multiplier in UQ16.16 (1/pitch), 0 = unshifted
    uint32_t period_scale;
    // The period registers as the track wrote them (low byte, high register incl. length bits)
    uint8_t track_period_lo;
    uint8_t track_period_hi;
    // The high period bits actually written to the hardware
    uint8_t hw_period_hi;
} SfxState;

struct FamPlayer {
    FamApu* apu;

    const FamTrack* music;
    MusicState music_state;
    // Starts when the current music ends by itself. Never set while no music is playing
    const FamTrack* queued_music;

    // Shadow state to track the music's "real" register state
    uint8_t reserve_pulse1[4];
    uint8_t reserve_pulse2[4];
    uint8_t reserve_triangle[4];
    uint8_t reserve_noise[4];
    uint8_t reserve_dmc[4];
    uint8_t reserve_status;

    const FamTrack* sfx[SFX_CHANNEL_COUNT];
    SfxState sfx_state[SFX_CHANNEL_COUNT];

    uint8_t last_status_written;

    // Last value the player wrote to each channel register $4000-$4013, like a real driver's
    // shadow copy. Lets a pitched sound effect know the channel's current period before it writes one
    uint8_t hw_registers[FAM_REGISTER_DMC_3 - FAM_REGISTER_PULSE1_0 + 1];
};

// Every register write the player makes goes through here
static void player_apu_write(FamPlayer* player, uint16_t reg, uint8_t data) {
    if (reg >= FAM_REGISTER_PULSE1_0 && reg <= FAM_REGISTER_DMC_3) {
        player->hw_registers[reg - FAM_REGISTER_PULSE1_0] = data;
    }
    fam_apu_write_register(player->apu, reg, data);
}

static void player_clear_reserve(FamPlayer* player) {
    memset(player->reserve_pulse1, 0, 4);
    memset(player->reserve_pulse2, 0, 4);
    memset(player->reserve_triangle, 0, 4);
    memset(player->reserve_noise, 0, 4);
    memset(player->reserve_dmc, 0, 4);
    player->reserve_status = 0;
}

// NOTE: This will retrigger notes by loading the length counters! Use only if necessary (Like after a sound effect stops)
// TODO: A more sophisticated function that avoids reloading length counters if possible
static void player_restore_reserve(FamPlayer* player, int channel) {
    switch(channel) {
        case FAM_CHANNEL_PULSE1:
            player_apu_write(player, 0x4000, player->reserve_pulse1[0]);
            player_apu_write(player, 0x4001, player->reserve_pulse1[1]);
            player_apu_write(player, 0x4002, player->reserve_pulse1[2]);
            player_apu_write(player, 0x4003, player->reserve_pulse1[3]);
            break;
        case FAM_CHANNEL_PULSE2:
            player_apu_write(player, 0x4004, player->reserve_pulse2[0]);
            player_apu_write(player, 0x4005, player->reserve_pulse2[1]);
            player_apu_write(player, 0x4006, player->reserve_pulse2[2]);
            player_apu_write(player, 0x4007, player->reserve_pulse2[3]);
            break;
        case FAM_CHANNEL_TRIANGLE:
            player_apu_write(player, 0x4008, player->reserve_triangle[0]);
            player_apu_write(player, 0x4009, player->reserve_triangle[1]);
            player_apu_write(player, 0x400A, player->reserve_triangle[2]);
            player_apu_write(player, 0x400B, player->reserve_triangle[3]);
            break;
        case FAM_CHANNEL_NOISE:
            player_apu_write(player, 0x400C, player->reserve_noise[0]);
            player_apu_write(player, 0x400D, player->reserve_noise[1]);
            player_apu_write(player, 0x400E, player->reserve_noise[2]);
            player_apu_write(player, 0x400F, player->reserve_noise[3]);
            break;
        case FAM_CHANNEL_DMC:
            player_apu_write(player, 0x4010, player->reserve_dmc[0]);
            player_apu_write(player, 0x4011, player->reserve_dmc[1]);
            player_apu_write(player, 0x4012, player->reserve_dmc[2]);
            player_apu_write(player, 0x4013, player->reserve_dmc[3]);
            break;
        default:
            break;
    }
}

static void player_update_status_register(FamPlayer* player, bool force) {
    uint8_t status = 0;

    if (player->music != NULL) {
        status |= player->reserve_status;
    }

    // A present SFX owns its channel whether it's been enabled or not
    for (int i = 0; i < SFX_CHANNEL_COUNT; i++) {
        if (player->sfx[i] != NULL) {
            status &= ~(1 << i);
            if (player->sfx_state[i].enabled) {
                status |= 1 << i;
            }
        }
    }

    // Re-asserting CHAN_BIT_DMC restarts a finished sample, so unless an explicit
    // OP_STATUS_WRITE forces it, leave bit 4 at whatever the DMC is actually doing.
    uint8_t status_read;
    fam_apu_read_register(player->apu, FAM_REGISTER_STATUS, &status_read);
    uint8_t to_write = force ? status
                             : (status & ~FAM_CHAN_BIT_DMC) | (status_read & FAM_CHAN_BIT_DMC);

    if (force || to_write != player->last_status_written) {
        player_apu_write(player, FAM_REGISTER_STATUS, to_write);
        player->last_status_written = to_write;
    }
}

static void player_silence_music(FamPlayer* player) {
    // Mute pulses and noise by setting volume to 0 ($4000, $4004, $400C)
    if (player->sfx[FAM_CHANNEL_PULSE1] == NULL) player_apu_write(player, 0x4000, 0x30);
    if (player->sfx[FAM_CHANNEL_PULSE2] == NULL) player_apu_write(player, 0x4004, 0x30);
    if (player->sfx[FAM_CHANNEL_NOISE] == NULL) player_apu_write(player, 0x400C, 0x30);
    // Mute triangle by setting linear counter to 0 and halting ($4008)
    if (player->sfx[FAM_CHANNEL_TRIANGLE] == NULL) player_apu_write(player, 0x4008, 0x80);
}

static uint8_t player_dmc_callback(void* user_data, uint16_t addr) {
    FamPlayer* player = (FamPlayer*)user_data;
    assert(player != NULL);

    if (player->music == NULL ||
        player->music_state.current_dpcm_bank_data == NULL) {
        return 0;
    }

    if (addr < 0xC000) {
        return 0;
    }

    uint32_t ind = addr - 0xC000;
    if (ind >= MUSIC_DPCM_SAMPLE_BANK_SIZE) {
        return 0;
    }

    return player->music_state.current_dpcm_bank_data[ind];
}

static void player_switch_music_dpcm_bank(FamPlayer* player, uint8_t index) {
    player->music_state.current_dpcm_bank_data = player->music_state.dpcm_bank_data_begin + (size_t)index * MUSIC_DPCM_SAMPLE_BANK_SIZE;
}

static uint32_t player_track_get_delta(const uint8_t* pos) {
    const uint8_t opcode = pos[0];
    if (opcode >= OP_WAIT) {
        return ((uint32_t)pos[1] | (uint32_t)pos[2] << 8 | (uint32_t)pos[3] << 16) + 1;
    }
    return ((uint32_t)pos[2] | (uint32_t)pos[3] << 8) + 1;
}

static FamResult player_check_music(const FamPlayer* player, const FamTrack* music) {
    if (fam_track_get_usage(music) != FAM_TRACK_USAGE_MUSIC) {
        return FAM_ERROR_USAGE_MISMATCH;
    }

    // Should I allow mismatching regions here? It would sound wrong, but not break anything...
    if (fam_track_get_region(music) != fam_apu_get_region(player->apu)) {
        return FAM_ERROR_REGION_MISMATCH;
    }

    return FAM_SUCCESS;
}

// Starts the music from the beginning, replacing the current music and clearing the queue.
// The music must have passed player_check_music
static void player_start_music(FamPlayer* player, const FamTrack* music) {
    const TrackInfo info = track_get_info((const uint8_t*)music);

    player->music = music;
    player->queued_music = NULL;

    player->music_state.channel_mask = info.channel_mask;
    player->music_state.dpcm_bank_count = info.music_dpcm_bank_count;

    player->music_state.data_begin = (const uint8_t*)music + (size_t)info.op_data_offset;
    player->music_state.data_end = player->music_state.data_begin + info.op_data_size;
    player->music_state.data_pos = player->music_state.data_begin;

    // A zero loop_offset means no looping
    player->music_state.data_loop = info.music_loop_offset ? (const uint8_t*)music + info.music_loop_offset : NULL;
    player->music_state.cycles_remaining = 0;

    player->music_state.dpcm_bank_data_begin = (const uint8_t*)music + info.music_dpcm_bank_data_offset;

    if (info.music_dpcm_bank_count > 0) {
        player_switch_music_dpcm_bank(player, 0);
    } else {
        player->music_state.current_dpcm_bank_data = NULL;
    }

    player->music_state.paused = false;

    // Reset reserve state
    player_clear_reserve(player);

    player_update_status_register(player, true);
}

static void player_process_music(FamPlayer* player, uint32_t cycles) {
    if (player->music == NULL) {
        return;
    }
    
    MusicState* state = &player->music_state;

    if (state->paused) {
        return;
    }

    if (cycles < state->cycles_remaining) {
        state->cycles_remaining -= cycles;
        return;
    }

    // NOTE: cycles is never larger than cycles_remaining
    // Therefore if it's not less than, it's exactly equal
    if (state->data_pos >= state->data_end) {
        // End of song reached
        if (state->data_loop == NULL) {
            // Stopping clears the queue, so take the queued music first
            const FamTrack* next = player->queued_music;
            fam_player_stop_music(player);

            if (next != NULL) {
                // Checked when it was queued. With cycles_remaining at 0, fam_player_run
                // processes its first op on the very same cycle, so there's no gap
                player_start_music(player, next);
            }
            return;
        }

        // Fall through to run the loop point op right away, with no wait
        state->data_pos = state->data_loop;
    }

    const uint8_t* op = state->data_pos;
    const uint8_t opcode = op[0];
    const uint8_t op_data = op[1];

    switch(opcode) {
        case OP_PULSE1_WRITE0:
        case OP_PULSE1_WRITE1:
        case OP_PULSE1_WRITE2:
        case OP_PULSE1_WRITE3:
            {
                if ((state->channel_mask & FAM_CHAN_BIT_PULSE1) == 0) break;

                int offset = opcode - OP_PULSE1_WRITE0;
                player->reserve_pulse1[offset] = op_data;
                if (player->sfx[FAM_CHANNEL_PULSE1] == NULL) {
                    player_apu_write(player, (uint16_t)(0x4000 + offset), op_data);
                }
                break;
            }
        case OP_PULSE2_WRITE0:
        case OP_PULSE2_WRITE1:
        case OP_PULSE2_WRITE2:
        case OP_PULSE2_WRITE3:
            {
                if ((state->channel_mask & FAM_CHAN_BIT_PULSE2) == 0) break;

                int offset = opcode - OP_PULSE2_WRITE0;
                player->reserve_pulse2[offset] = op_data;
                if (player->sfx[FAM_CHANNEL_PULSE2] == NULL) {
                    player_apu_write(player, (uint16_t)(0x4004 + offset), op_data);
                }
                break;
            }
        case OP_TRIANGLE_WRITE0:
        case OP_TRIANGLE_WRITE1:
        case OP_TRIANGLE_WRITE2:
        case OP_TRIANGLE_WRITE3:
            {
                if ((state->channel_mask & FAM_CHAN_BIT_TRIANGLE) == 0) break;

                int offset = opcode - OP_TRIANGLE_WRITE0;
                player->reserve_triangle[offset] = op_data;
                if (player->sfx[FAM_CHANNEL_TRIANGLE] == NULL) {
                    player_apu_write(player, (uint16_t)(0x4008 + offset), op_data);
                }
                break;
            }
        case OP_NOISE_WRITE0:
        case OP_NOISE_WRITE1:
        case OP_NOISE_WRITE2:
        case OP_NOISE_WRITE3:
            {
                if ((state->channel_mask & FAM_CHAN_BIT_NOISE) == 0) break;

                int offset = opcode - OP_NOISE_WRITE0;
                player->reserve_noise[offset] = op_data;
                if (player->sfx[FAM_CHANNEL_NOISE] == NULL) {
                    player_apu_write(player, (uint16_t)(0x400C + offset), op_data);
                }
                break;
            }
        case OP_DMC_WRITE0:
        case OP_DMC_WRITE1:
        case OP_DMC_WRITE2:
        case OP_DMC_WRITE3:
            {
                if ((state->channel_mask & FAM_CHAN_BIT_DMC) == 0) break;

                int offset = opcode - OP_DMC_WRITE0;
                player->reserve_dmc[offset] = op_data;
                player_apu_write(player, (uint16_t)(0x4010 + offset), op_data);
                break;
            }
        case OP_STATUS_WRITE:
            player->reserve_status = (uint8_t)(op_data & state->channel_mask & CHANNEL_MASK_APU_STATUS);
            player_update_status_register(player, true);
            break;
        case OP_SWITCH_SAMPLE_BANK:
            if ((state->channel_mask & FAM_CHAN_BIT_DMC) == 0) break;

            uint8_t bank_index = op_data;

            if (bank_index >= state->dpcm_bank_count) {
                state->current_dpcm_bank_data = NULL;
                break;
            }

            player_switch_music_dpcm_bank(player, bank_index);

            break;
        case OP_WAIT:
            // Carries nothing but a wider delta, so there is nothing to apply
            break;

        default:
            break;
    }

    state->cycles_remaining = player_track_get_delta(state->data_pos);
    state->data_pos += TRACK_OP_SIZE;
}

// Ends the sound effect on the channel and hands the channel back to the music
static void player_release_sfx(FamPlayer* player, int channel) {
    player->sfx[channel] = NULL;
    memset(&player->sfx_state[channel], 0, sizeof(SfxState));
    player_update_status_register(player, false);
    player_restore_reserve(player, channel);

    // Re-mute paused music after reserve restore
    if (player->music_state.paused) {
        player_silence_music(player);
    }
}

// --- Sound effect pitch shift ---
// Works like a real driver applying a pitch offset in code: the track stays untouched, and the
// player transforms period values on their way to the APU.

#define PERIOD_MAX 0x7FF
// Below 8 the pulse channels are silenced by the hardware
#define PULSE_PERIOD_MIN 8
// Periods 0 and 1 make the triangle ultrasonic, which is heard as a pop
#define TRIANGLE_PERIOD_MIN 2

// Frequency is clock / (k * (t + 1)), so dividing the frequency by pitch means multiplying (t + 1)
static uint16_t player_shift_period(uint16_t period, uint32_t scale, uint16_t min_period) {
    const uint64_t scaled = (((uint64_t)period + 1) * scale + 0x8000) >> 16;
    const uint64_t shifted = scaled > 0 ? scaled - 1 : 0;

    if (shifted < min_period) return min_period;
    if (shifted > PERIOD_MAX) return PERIOD_MAX;
    return (uint16_t)shifted;
}

// The period is 11 bits split over two registers, and writing the high one has side effects
// (pulse: phase reset, envelope restart and length reload; triangle: linear counter reload).
// Tracks write only the low byte for slides and vibrato to avoid them. So a shifted sound effect
// never writes the high register unless the track did: if the shifted period leaves the page the
// hardware is on, the low byte saturates instead. The shifted sound effect then has exactly the
// same hardware side effects as the original, at the cost of a pitch error while crossing a page.
// Writing the low register has no side effects, so it's always written along with the high one.
static void player_write_sfx_period(FamPlayer* player, SfxState* state, uint16_t base_reg, int offset, uint8_t data, uint16_t min_period) {
    if (offset == 2) {
        state->track_period_lo = data;
    } else {
        state->track_period_hi = data;
    }

    const uint16_t period = (uint16_t)((state->track_period_hi & 0x07) << 8 | state->track_period_lo);
    const uint16_t shifted = player_shift_period(period, state->period_scale, min_period);
    const uint8_t shifted_hi = (uint8_t)(shifted >> 8);

    if (offset == 3) {
        player_apu_write(player, (uint16_t)(base_reg + 2), (uint8_t)shifted);
        // Keep the track's length counter load bits
        player_apu_write(player, (uint16_t)(base_reg + 3), (uint8_t)((data & 0xF8) | shifted_hi));
        state->hw_period_hi = shifted_hi;
        return;
    }

    uint8_t lo = (uint8_t)shifted;
    if (shifted_hi > state->hw_period_hi) {
        lo = 0xFF;
    } else if (shifted_hi < state->hw_period_hi) {
        lo = 0x00;
    }
    player_apu_write(player, (uint16_t)(base_reg + 2), lo);
}

// Noise has 16 fixed periods, so the closest one is used. Closest by ratio rather than difference,
// since pitch is perceived logarithmically. Coarse: small shifts usually keep the same period
static uint8_t player_shift_noise_index(const uint16_t* periods, uint8_t index, uint32_t scale) {
    // Target period in UQ16.16
    const uint64_t target = (uint64_t)periods[index] * scale;

    if (target <= (uint64_t)periods[0] << 16) return 0;
    if (target >= (uint64_t)periods[15] << 16) return 15;

    // The table is ascending: find the first period at or above the target, then pick between it
    // and the one below. Closer in ratio means target is on that side of their geometric mean:
    // target / lo < hi / target  <=>  target^2 < lo * hi. Both sides fit in 64 bits (< 2^54)
    uint8_t hi = 1;
    while (((uint64_t)periods[hi] << 16) < target) {
        hi++;
    }
    const uint64_t lo_hi = ((uint64_t)periods[hi - 1] * periods[hi]) << 32;
    return (target * target < lo_hi) ? (uint8_t)(hi - 1) : hi;
}

static void player_process_sfx(FamPlayer* player, int channel, uint32_t cycles) {
    if (player->sfx[channel] == NULL) {
        return;
    }

    SfxState* state = &player->sfx_state[channel];

    if (cycles < state->cycles_remaining) {
        state->cycles_remaining -= cycles;
        return;
    }

    if (state->data_pos >= state->data_end) {
        // A sound effect never loops, so running out of ops ends it
        player_release_sfx(player, channel);
        return;
    }

    const uint8_t* op = state->data_pos;
    const uint8_t opcode = op[0];
    const uint8_t op_data = op[1];

    switch(opcode) {
        case OP_PULSE1_WRITE0:
        case OP_PULSE1_WRITE1:
        case OP_PULSE1_WRITE2:
        case OP_PULSE1_WRITE3:
            {
                if (channel != FAM_CHANNEL_PULSE1) break;

                int offset = opcode - OP_PULSE1_WRITE0;
                if (state->period_scale != 0 && offset >= 2) {
                    player_write_sfx_period(player, state, FAM_REGISTER_PULSE1_0, offset, op_data, PULSE_PERIOD_MIN);
                    break;
                }
                player_apu_write(player, (uint16_t)(0x4000 + offset), op_data);
                break;
            }
        case OP_PULSE2_WRITE0:
        case OP_PULSE2_WRITE1:
        case OP_PULSE2_WRITE2:
        case OP_PULSE2_WRITE3:
            {
                if (channel != FAM_CHANNEL_PULSE2) break;

                int offset = opcode - OP_PULSE2_WRITE0;
                if (state->period_scale != 0 && offset >= 2) {
                    player_write_sfx_period(player, state, FAM_REGISTER_PULSE2_0, offset, op_data, PULSE_PERIOD_MIN);
                    break;
                }
                player_apu_write(player, (uint16_t)(0x4004 + offset), op_data);
                break;
            }
        case OP_TRIANGLE_WRITE0:
        case OP_TRIANGLE_WRITE1:
        case OP_TRIANGLE_WRITE2:
        case OP_TRIANGLE_WRITE3:
            {
                if (channel != FAM_CHANNEL_TRIANGLE) break;

                int offset = opcode - OP_TRIANGLE_WRITE0;
                if (state->period_scale != 0 && offset >= 2) {
                    player_write_sfx_period(player, state, FAM_REGISTER_TRIANGLE_0, offset, op_data, TRIANGLE_PERIOD_MIN);
                    break;
                }
                player_apu_write(player, (uint16_t)(0x4008 + offset), op_data);
                break;
            }
        case OP_NOISE_WRITE0:
        case OP_NOISE_WRITE1:
        case OP_NOISE_WRITE2:
        case OP_NOISE_WRITE3:
            {
                if (channel != FAM_CHANNEL_NOISE) break;

                int offset = opcode - OP_NOISE_WRITE0;
                uint8_t data = op_data;
                if (state->period_scale != 0 && offset == 2) {
                    // Mode flag in bit 7 is kept, only the period index in bits 0-3 is shifted
                    const uint16_t* periods = NOISE_PERIOD[fam_apu_get_region(player->apu)];
                    data = (uint8_t)((op_data & 0xF0) | player_shift_noise_index(periods, op_data & 0x0F, state->period_scale));
                }
                player_apu_write(player, (uint16_t)(0x400C + offset), data);
                break;
            }
        case OP_DMC_WRITE0:
        case OP_DMC_WRITE1:
        case OP_DMC_WRITE2:
        case OP_DMC_WRITE3:
        case OP_SWITCH_SAMPLE_BANK:
            break;
        case OP_STATUS_WRITE:
            state->enabled = (op_data >> channel) & 1;
            player_update_status_register(player, false);
            break;

        case OP_WAIT:
            break;

        default:
            break;
    }

    state->cycles_remaining = player_track_get_delta(state->data_pos);
    state->data_pos += TRACK_OP_SIZE;
}

static void player_process_cycles(FamPlayer* player, uint32_t cycles) {
    player_process_music(player, cycles);
    
    for (int i = 0; i < SFX_CHANNEL_COUNT; i++) {
        player_process_sfx(player, i, cycles);
    }
}

size_t fam_player_get_memory_required(void) {
    return sizeof(FamPlayer);
}

size_t fam_player_get_memory_alignment(void) {
    return alignof(FamPlayer);
}

FamResult fam_player_init(FamPlayer** out_player, void* memory, FamApu* apu) {
    if (out_player == NULL || apu == NULL || memory == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FamPlayer* player = (FamPlayer*)memory;
    memset(player, 0, sizeof(FamPlayer));

    player->apu = apu;

    fam_apu_set_dmc_reader(apu, player_dmc_callback, player);
    // 4-step sequence, disable IRQ
    player_apu_write(player, FAM_REGISTER_FRAME_COUNTER, 0x40);

    // Init reserve state
    player_clear_reserve(player);

    *out_player = player;
    return FAM_SUCCESS;
}

void fam_player_shutdown(FamPlayer* player) {
    assert(player != NULL);

    if (player->apu != NULL) {
        fam_apu_set_dmc_reader(player->apu, NULL, NULL);
        player->apu = NULL;
    }
}

void fam_player_run(FamPlayer* player, size_t sample_count, void* out_samples) {
    assert(player != NULL);
    assert(out_samples != NULL);
    assert(sample_count >= 0);

    if (sample_count == 0) {
        return;
    }

    size_t cycles_remaining = fam_apu_get_cycles_for_samples(player->apu, sample_count);
    const size_t stride = fam_apu_get_sample_size(player->apu);
    size_t samples_offset = 0;

    while (cycles_remaining > 0) {
        uint32_t max_cycles = (uint32_t)cycles_remaining;

        if (player->music != NULL && !player->music_state.paused &&
            player->music_state.cycles_remaining < max_cycles) {
            max_cycles = player->music_state.cycles_remaining;
        }
        for (int i = 0; i < SFX_CHANNEL_COUNT; i++) {
            if (player->sfx[i] != NULL && player->sfx_state[i].cycles_remaining < max_cycles) {
                max_cycles = player->sfx_state[i].cycles_remaining;
            }
        }

        if (max_cycles > 0) {
            size_t processed = fam_apu_run(player->apu, max_cycles, (uint8_t*)out_samples + samples_offset);
            samples_offset += processed * stride;
        }

        player_process_cycles(player, max_cycles);

        cycles_remaining -= max_cycles;
    }
}

FamResult fam_player_play_music(FamPlayer* player, const FamTrack* music) {
    assert(player != NULL);
    assert(music != NULL);

    const FamResult result = player_check_music(player, music);
    if (result != FAM_SUCCESS) {
        return result;
    }

    player_start_music(player, music);
    return FAM_SUCCESS;
}

FamResult fam_player_queue_music(FamPlayer* player, const FamTrack* music) {
    assert(player != NULL);

    if (music == NULL) {
        player->queued_music = NULL;
        return FAM_SUCCESS;
    }

    const FamResult result = player_check_music(player, music);
    if (result != FAM_SUCCESS) {
        return result;
    }

    if (player->music == NULL) {
        player_start_music(player, music);
    } else {
        player->queued_music = music;
    }

    return FAM_SUCCESS;
}

const FamTrack* fam_player_get_queued_music(const FamPlayer* player) {
    assert(player != NULL);
    return player->queued_music;
}

void fam_player_pause_music(FamPlayer* player) {
    assert(player != NULL);

    if (player->music == NULL || player->music_state.paused) {
        return;  
    }

    player->music_state.paused = true;
    player_silence_music(player);
}

void fam_player_resume_music(FamPlayer* player) {
    assert(player != NULL);

    if (player->music == NULL || !player->music_state.paused) {
        return;
    }

    player->music_state.paused = false;

    // Restore pulse and noise volume, triangle linear counter and halt
    if (player->sfx[FAM_CHANNEL_PULSE1] == NULL) player_apu_write(player, 0x4000, player->reserve_pulse1[0]);
    if (player->sfx[FAM_CHANNEL_PULSE2] == NULL) player_apu_write(player, 0x4004, player->reserve_pulse2[0]);
    if (player->sfx[FAM_CHANNEL_TRIANGLE] == NULL) player_apu_write(player, 0x4008, player->reserve_triangle[0]);
    if (player->sfx[FAM_CHANNEL_NOISE] == NULL) player_apu_write(player, 0x400C, player->reserve_noise[0]);
}

void fam_player_stop_music(FamPlayer* player) {
    assert(player != NULL);

    player->queued_music = NULL;

    if (player->music == NULL) {
        return;
    }

    player->music = NULL;
    memset(&player->music_state, 0, sizeof(MusicState));

    player_update_status_register(player, false);
}

const FamTrack* fam_player_get_music(const FamPlayer* player) {
    assert(player != NULL);
    return player->music;
}

FamPlaybackState fam_player_get_music_state(const FamPlayer* player) {
    assert(player != NULL);

    if (player->music == NULL) {
        return FAM_PLAYBACK_STOPPED;
    }

    return player->music_state.paused ? FAM_PLAYBACK_PAUSED : FAM_PLAYBACK_PLAYING;
}

// period_scale is in UQ16.16, 0 means unshifted
static FamResult player_play_sfx(FamPlayer* player, const FamTrack* sfx, uint32_t period_scale) {
    const TrackInfo info = track_get_info((const uint8_t*)sfx);

    if (info.usage != FAM_TRACK_USAGE_SFX) {
        return FAM_ERROR_USAGE_MISMATCH;
    }

    if (info.region != fam_apu_get_region(player->apu)) {
        return FAM_ERROR_REGION_MISMATCH;
    }

    // Validated at load time to be a single SFX-capable channel
    const FamChannel channel = fam_track_get_sfx_channel(sfx);
    assert(channel >= 0 && channel < SFX_CHANNEL_COUNT);

    player->sfx[channel] = sfx;

    SfxState* state = &player->sfx_state[channel];
    memset(state, 0, sizeof(SfxState));
    state->data_begin = (const uint8_t*)sfx + (size_t)info.op_data_offset;
    state->data_end = state->data_begin + info.op_data_size;
    state->data_pos = state->data_begin;

    // Until the track writes its own period, it plays on whatever the channel's registers hold
    // (left by the music or an earlier sound effect), so start from the hardware's current period.
    // Channel registers are 4 apart in FamChannel order, so they're found by index
    state->period_scale = period_scale;
    state->track_period_lo = player->hw_registers[channel * 4 + 2];
    state->track_period_hi = player->hw_registers[channel * 4 + 3];
    state->hw_period_hi = state->track_period_hi & 0x07;

    player_update_status_register(player, false);

    return FAM_SUCCESS;
}

FamResult fam_player_play_sfx(FamPlayer* player, const FamTrack* sfx) {
    assert(player != NULL);
    assert(sfx != NULL);

    return player_play_sfx(player, sfx, 0);
}

FamResult fam_player_play_sfx_pitched(FamPlayer* player, const FamTrack* sfx, float pitch) {
    assert(player != NULL);
    assert(sfx != NULL);

    // Written this way round so NaN is rejected too
    if (!(pitch > 0.0f)) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    // Period multiplier 1/pitch in UQ16.16. Extremes clamp, the periods are clamped anyway
    const double scale = 65536.0 / (double)pitch;
    uint32_t period_scale;
    if (scale < 1.0) {
        period_scale = 1;
    } else if (scale > (double)0x7FFFFFFF) {
        period_scale = 0x7FFFFFFF;
    } else {
        period_scale = (uint32_t)(scale + 0.5);
    }

    // An exact 1.0 takes the unshifted path, so it writes exactly what fam_player_play_sfx would
    if (period_scale == 0x10000) {
        period_scale = 0;
    }

    return player_play_sfx(player, sfx, period_scale);
}

void fam_player_stop_sfx(FamPlayer* player, FamChannel channel) {
    assert(player != NULL);
    assert(channel == FAM_CHANNEL_NONE || (unsigned)channel < CHANNEL_COUNT);

    if (channel == FAM_CHANNEL_NONE || channel >= SFX_CHANNEL_COUNT || player->sfx[channel] == NULL) {
        return;
    }

    player_release_sfx(player, channel);
}

void fam_player_stop_all_sfx(FamPlayer* player) {
    assert(player != NULL);

    for (int i = 0; i < SFX_CHANNEL_COUNT; i++) {
        if (player->sfx[i] != NULL) {
            player_release_sfx(player, i);
        }
    }
}

const FamTrack* fam_player_get_sfx(const FamPlayer* player, FamChannel channel) {
    assert(player != NULL);
    assert(channel == FAM_CHANNEL_NONE || (unsigned)channel < CHANNEL_COUNT);

    if (channel == FAM_CHANNEL_NONE || channel >= SFX_CHANNEL_COUNT) {
        return NULL;
    }

    return player->sfx[channel];
}

void fam_player_stop_track(FamPlayer* player, const FamTrack* track) {
    assert(player != NULL);
    assert(track != NULL);

    if (player->queued_music == track) {
        player->queued_music = NULL;
    }

    if (player->music == track) {
        fam_player_stop_music(player);
    }

    for (int i = 0; i < SFX_CHANNEL_COUNT; i++) {
        if (player->sfx[i] == track) {
            player_release_sfx(player, i);
        }
    }
}
