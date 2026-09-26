#include <fam/player.h>
#include <fam/apu.h>
#include <fam/internal/stream.h>
#include <string.h>
#include <stdalign.h>

typedef struct MusicState {
    uint64_t channel_mask;
    uint16_t dpcm_bank_count;

    const uint8_t* data_begin;
    const uint8_t* data_end;
    const uint8_t* data_pos;
    const uint8_t* data_loop;
    uint8_t skip_counter;

    const uint8_t* dpcm_bank_data_begin;
    const uint8_t* current_dpcm_bank_data;
    
    bool paused;
} MusicState;

typedef struct SfxState {
    const uint8_t* data_begin;
    const uint8_t* data_end;
    const uint8_t* data_pos;
    uint8_t skip_counter;

    bool enabled; // Affects APU channel status but not channel ownership
} SfxState;

struct FamPlayer {
    FamApu* apu;
    uint32_t sample_rate;
    uint8_t format;

    const FamMusic* music;
    MusicState music_state;

    // Shadow state to track the music's "real" register state
    uint8_t reserve_pulse1[4];
    uint8_t reserve_pulse2[4];
    uint8_t reserve_triangle[4];
    uint8_t reserve_noise[4];
    uint8_t reserve_dmc[4];
    uint8_t reserve_status;

    const FamSfx* sfx[SFX_CHANNEL_COUNT];
    SfxState sfx_state[SFX_CHANNEL_COUNT];

    uint8_t last_status_written;

    double accumulator;
    double cycle_counter;
};

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
        case CHAN_ID_PULSE1:
            fam_apu_write_register(player->apu, 0x4000, player->reserve_pulse1[0]);
            fam_apu_write_register(player->apu, 0x4001, player->reserve_pulse1[1]);
            fam_apu_write_register(player->apu, 0x4002, player->reserve_pulse1[2]);
            fam_apu_write_register(player->apu, 0x4003, player->reserve_pulse1[3]);
            break;
        case CHAN_ID_PULSE2:
            fam_apu_write_register(player->apu, 0x4004, player->reserve_pulse2[0]);
            fam_apu_write_register(player->apu, 0x4005, player->reserve_pulse2[1]);
            fam_apu_write_register(player->apu, 0x4006, player->reserve_pulse2[2]);
            fam_apu_write_register(player->apu, 0x4007, player->reserve_pulse2[3]);
            break;
        case CHAN_ID_TRIANGLE:
            fam_apu_write_register(player->apu, 0x4008, player->reserve_triangle[0]);
            fam_apu_write_register(player->apu, 0x4009, player->reserve_triangle[1]);
            fam_apu_write_register(player->apu, 0x400A, player->reserve_triangle[2]);
            fam_apu_write_register(player->apu, 0x400B, player->reserve_triangle[3]);
            break;
        case CHAN_ID_NOISE:
            fam_apu_write_register(player->apu, 0x400C, player->reserve_noise[0]);
            fam_apu_write_register(player->apu, 0x400D, player->reserve_noise[1]);
            fam_apu_write_register(player->apu, 0x400E, player->reserve_noise[2]);
            fam_apu_write_register(player->apu, 0x400F, player->reserve_noise[3]);
            break;
        case CHAN_ID_DMC:
            fam_apu_write_register(player->apu, 0x4010, player->reserve_dmc[0]);
            fam_apu_write_register(player->apu, 0x4011, player->reserve_dmc[1]);
            fam_apu_write_register(player->apu, 0x4012, player->reserve_dmc[2]);
            fam_apu_write_register(player->apu, 0x4013, player->reserve_dmc[3]);
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
                             : (status & ~CHAN_BIT_DMC) | (status_read & CHAN_BIT_DMC);

    if (force || to_write != player->last_status_written) {
        fam_apu_write_register(player->apu, FAM_REGISTER_STATUS, to_write);
        player->last_status_written = to_write;
    }
}

static void player_silence_music(FamPlayer* player) {
    // Mute pulses and noise by setting volume to 0 ($4000, $4004, $400C)
    if (player->sfx[CHAN_ID_PULSE1] == NULL) fam_apu_write_register(player->apu, 0x4000, 0x30);
    if (player->sfx[CHAN_ID_PULSE2] == NULL) fam_apu_write_register(player->apu, 0x4004, 0x30);
    if (player->sfx[CHAN_ID_NOISE] == NULL) fam_apu_write_register(player->apu, 0x400C, 0x30);
    // Mute triangle by setting linear counter to 0 and halting ($4008)
    if (player->sfx[CHAN_ID_TRIANGLE] == NULL) fam_apu_write_register(player->apu, 0x4008, 0x80);
}

static uint8_t player_dmc_callback(void* user_data, uint16_t addr) {
    FamPlayer* player = (FamPlayer*)user_data;
    if (player == NULL || 
        player->music == NULL ||
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

static void player_process_music(FamPlayer* player) {
    if (player->music == NULL) {
        return;
    }
    
    MusicState* state = &player->music_state;

    if (state->paused) {
        return;
    }

    if (state->skip_counter > 0) {
        state->skip_counter--;
        return;
    }

loop:

    while (state->data_pos < state->data_end) {
        uint8_t opcode = *state->data_pos++;
        uint8_t op_data = *state->data_pos++;

        switch(opcode) {
            case OP_PULSE1_WRITE0:
            case OP_PULSE1_WRITE1:
            case OP_PULSE1_WRITE2:
            case OP_PULSE1_WRITE3:
                {
                    if ((state->channel_mask & CHAN_BIT_PULSE1) == 0) break;

                    int offset = opcode - OP_PULSE1_WRITE0;
                    player->reserve_pulse1[offset] = op_data;
                    if (player->sfx[CHAN_ID_PULSE1] == NULL) {
                        fam_apu_write_register(player->apu, (uint16_t)(0x4000 + offset), op_data);
                    }
                    break;
                }
            case OP_PULSE2_WRITE0:
            case OP_PULSE2_WRITE1:
            case OP_PULSE2_WRITE2:
            case OP_PULSE2_WRITE3:
                {
                    if ((state->channel_mask & CHAN_BIT_PULSE2) == 0) break;

                    int offset = opcode - OP_PULSE2_WRITE0;
                    player->reserve_pulse2[offset] = op_data;
                    if (player->sfx[CHAN_ID_PULSE2] == NULL) {
                        fam_apu_write_register(player->apu, (uint16_t)(0x4004 + offset), op_data);
                    }
                    break;
                }
            case OP_TRIANGLE_WRITE0:
            case OP_TRIANGLE_WRITE1:
            case OP_TRIANGLE_WRITE2:
            case OP_TRIANGLE_WRITE3:
                {
                    if ((state->channel_mask & CHAN_BIT_TRIANGLE) == 0) break;

                    int offset = opcode - OP_TRIANGLE_WRITE0;
                    player->reserve_triangle[offset] = op_data;
                    if (player->sfx[CHAN_ID_TRIANGLE] == NULL) {
                        fam_apu_write_register(player->apu, (uint16_t)(0x4008 + offset), op_data);
                    }
                    break;
                }
            case OP_NOISE_WRITE0:
            case OP_NOISE_WRITE1:
            case OP_NOISE_WRITE2:
            case OP_NOISE_WRITE3:
                {
                    if ((state->channel_mask & CHAN_BIT_NOISE) == 0) break;

                    int offset = opcode - OP_NOISE_WRITE0;
                    player->reserve_noise[offset] = op_data;
                    if (player->sfx[CHAN_ID_NOISE] == NULL) {
                        fam_apu_write_register(player->apu, (uint16_t)(0x400C + offset), op_data);
                    }
                    break;
                }
            case OP_DMC_WRITE0:
            case OP_DMC_WRITE1:
            case OP_DMC_WRITE2:
            case OP_DMC_WRITE3:
                {
                    if ((state->channel_mask & CHAN_BIT_DMC) == 0) break;

                    int offset = opcode - OP_DMC_WRITE0;
                    player->reserve_dmc[offset] = op_data;
                    fam_apu_write_register(player->apu, (uint16_t)(0x4010 + offset), op_data);
                    break;
                }
            case OP_STATUS_WRITE:
                player->reserve_status = (uint8_t)(op_data & state->channel_mask & CHANNEL_MASK_APU_STATUS);
                player_update_status_register(player, true);
                break;
            case OP_SWITCH_SAMPLE_BANK:
                if ((state->channel_mask & CHAN_BIT_DMC) == 0) break;

                uint8_t bank_index = op_data;

                if (bank_index >= state->dpcm_bank_count) {
                    state->current_dpcm_bank_data = NULL;
                    break;
                }

                player_switch_music_dpcm_bank(player, bank_index);

                break;
            case OP_ENDFRAME:
                state->skip_counter = op_data;
                return;
            
            case OP_ENDSTREAM:
                goto endstream;

            default:
                break;
        }
    }

endstream:

    // End of song reached
    if (state->data_loop == NULL) {
        fam_player_stop_music(player);
    } else {
        state->data_pos = state->data_loop;
        goto loop;
    }
}

static void player_process_sfx(FamPlayer* player, int channel) {
    if (player->sfx[channel] == NULL) {
        return;
    }

    SfxState* state = &player->sfx_state[channel];

    if (state->skip_counter > 0) {
        state->skip_counter--;
        return;
    }

    while (state->data_pos < state->data_end) {
        uint8_t opcode = *state->data_pos++;
        uint8_t op_data = *state->data_pos++;

        switch(opcode) {
            case OP_PULSE1_WRITE0:
            case OP_PULSE1_WRITE1:
            case OP_PULSE1_WRITE2:
            case OP_PULSE1_WRITE3:
                {
                    if (channel != CHAN_ID_PULSE1) break;

                    int offset = opcode - OP_PULSE1_WRITE0;
                    fam_apu_write_register(player->apu, (uint16_t)(0x4000 + offset), op_data);
                    break;
                }
            case OP_PULSE2_WRITE0:
            case OP_PULSE2_WRITE1:
            case OP_PULSE2_WRITE2:
            case OP_PULSE2_WRITE3:
                {
                    if (channel != CHAN_ID_PULSE2) break;

                    int offset = opcode - OP_PULSE2_WRITE0;
                    fam_apu_write_register(player->apu, (uint16_t)(0x4004 + offset), op_data);
                    break;
                }
            case OP_TRIANGLE_WRITE0:
            case OP_TRIANGLE_WRITE1:
            case OP_TRIANGLE_WRITE2:
            case OP_TRIANGLE_WRITE3:
                {
                    if (channel != CHAN_ID_TRIANGLE) break;

                    int offset = opcode - OP_TRIANGLE_WRITE0;
                    fam_apu_write_register(player->apu, (uint16_t)(0x4008 + offset), op_data);
                    break;
                }
            case OP_NOISE_WRITE0:
            case OP_NOISE_WRITE1:
            case OP_NOISE_WRITE2:
            case OP_NOISE_WRITE3:
                {
                    if (channel != CHAN_ID_NOISE) break;

                    int offset = opcode - OP_NOISE_WRITE0;
                    fam_apu_write_register(player->apu, (uint16_t)(0x400C + offset), op_data);
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

            case OP_ENDFRAME:
                state->skip_counter = op_data;
                return;
            
            case OP_ENDSTREAM:
                goto endstream;
            
            default:
                break;
        }
    }

endstream:

    player->sfx[channel] = NULL;
    memset(state, 0, sizeof(SfxState));
    player_update_status_register(player, false);
    player_restore_reserve(player, channel);

    // Re-mute paused music after reserve restore
    if (player->music_state.paused) {
        player_silence_music(player);
    }
}

static void player_process_frame(FamPlayer* player) {
    player_process_music(player);
    
    for (int i = 0; i < SFX_CHANNEL_COUNT; i++) {
        player_process_sfx(player, i);
    }
}

size_t fam_player_get_memory_required(void) {
    return sizeof(FamPlayer);
}

size_t fam_player_get_memory_alignment(void) {
    return alignof(FamPlayer);
}

FamResult fam_player_init(FamPlayer** out_player, void* memory, FamApu* apu, uint32_t sample_rate, FamAudioFormat format) {
    if (out_player == NULL || apu == NULL || memory == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    if (sample_rate == 0) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FamPlayer* player = (FamPlayer*)memory;
    memset(player, 0, sizeof(FamPlayer));

    player->apu = apu;
    player->sample_rate = sample_rate;
    player->format = format;

    memset((void*)player->sfx, 0, sizeof(FamSfx*) * SFX_CHANNEL_COUNT);

    fam_apu_set_dmc_reader(apu, player_dmc_callback, player);
    // 4-step sequence, disable IRQ
    fam_apu_write_register(player->apu, FAM_REGISTER_FRAME_COUNTER, 0x40);

    // Init reserve state
    player_clear_reserve(player);

    *out_player = player;
    return FAM_SUCCESS;
}

void fam_player_shutdown(FamPlayer* player) {
    if (player == NULL) {
        return;
    }

    if (player->apu != NULL) {
        fam_apu_set_dmc_reader(player->apu, NULL, NULL);
        player->apu = NULL;
    }
}

FamResult fam_player_process_samples(FamPlayer* player, int sample_count, void* out_samples) {
    if (sample_count == 0) {
        return FAM_SUCCESS;
    }

    if (player == NULL || out_samples == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    // NOTE: Only float output supported atm
    float* samples = (float*)out_samples;

    const double apu_period = 1.0 / fam_apu_get_freq(player->apu);
    const double sample_time = 1.0 / (double)player->sample_rate;
    const double frame_cycles = fam_apu_get_frame_cycles(player->apu);

    for (int i = 0; i < sample_count; i++) {
        player->accumulator += sample_time;
        while (player->accumulator >= apu_period) {
            fam_apu_clock(player->apu);
            player->accumulator -= apu_period;
            player->cycle_counter++;

            if (player->cycle_counter >= frame_cycles) {
                player_process_frame(player);
                player->cycle_counter -= frame_cycles;
            }
        }

        // TODO: Average samples across multiple APU clocks to prevent aliasing
        fam_apu_get_sample(player->apu, samples + i);
    }

    return FAM_SUCCESS;
}

FamResult fam_player_play_music(FamPlayer* player, const FamMusic* music) {
    if (player == NULL || music == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    const StreamInfo info = stream_get_info((const uint8_t*)music);

    // Should I allow mismatching regions here? It would sound wrong, but not break anything...
    if (info.region != fam_apu_get_region(player->apu)) {
        return FAM_ERROR_REGION_MISMATCH;
    }

    player->music = music;

    player->music_state.channel_mask = info.channel_mask;
    player->music_state.dpcm_bank_count = info.music_dpcm_bank_count;

    player->music_state.data_begin = (const uint8_t*)music + (size_t)info.op_data_offset;
    player->music_state.data_end = player->music_state.data_begin + info.op_data_size;
    player->music_state.data_pos = player->music_state.data_begin;

    // A zero loop_offset means no looping
    player->music_state.data_loop = info.music_loop_offset ? (const uint8_t*)music + info.music_loop_offset : NULL;
    player->music_state.skip_counter = 0;

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

    return FAM_SUCCESS;
}

void fam_player_pause_music(FamPlayer* player) {
    if (player == NULL) {
        return;
    }

    if (player->music == NULL || player->music_state.paused) {
        return;  
    }

    player->music_state.paused = true;
    player_silence_music(player);
}

void fam_player_resume_music(FamPlayer* player) {
    if (player == NULL) {
        return;
    }

    if (player->music == NULL || !player->music_state.paused) {
        return;
    }

    player->music_state.paused = false;

    // Restore pulse and noise volume, triangle linear counter and halt
    if (player->sfx[CHAN_ID_PULSE1] == NULL) fam_apu_write_register(player->apu, 0x4000, player->reserve_pulse1[0]);
    if (player->sfx[CHAN_ID_PULSE2] == NULL) fam_apu_write_register(player->apu, 0x4004, player->reserve_pulse2[0]);
    if (player->sfx[CHAN_ID_TRIANGLE] == NULL) fam_apu_write_register(player->apu, 0x4008, player->reserve_triangle[0]);
    if (player->sfx[CHAN_ID_NOISE] == NULL) fam_apu_write_register(player->apu, 0x400C, player->reserve_noise[0]);
}

void fam_player_stop_music(FamPlayer* player) {
    if (player == NULL) {
        return;
    }

    if (player->music == NULL) {
        return;
    }

    player->music = NULL;
    memset(&player->music_state, 0, sizeof(MusicState));

    player_update_status_register(player, false);
}

FamResult fam_player_play_sfx(FamPlayer* player, const FamSfx* sfx) {
    if (player == NULL || sfx == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    const StreamInfo info = stream_get_info((const uint8_t*)sfx);

    if (info.region != fam_apu_get_region(player->apu)) {
        return FAM_ERROR_REGION_MISMATCH;
    }

    // A sound effect with no channel is an allowed no-op
    // SFX can only play on a single channel, so if the mask contains multiple for some reason,
    // only the lowest is used
    for (uint8_t channel = 0; channel < SFX_CHANNEL_COUNT; channel++) {
        if ((info.channel_mask >> channel & 1) == 0) {
            continue;
        }

        player->sfx[channel] = sfx;

        player->sfx_state[channel].data_begin = (const uint8_t*)sfx + (size_t)info.op_data_offset;
        player->sfx_state[channel].data_end = player->sfx_state[channel].data_begin + info.op_data_size;
        player->sfx_state[channel].data_pos = player->sfx_state[channel].data_begin;
        player->sfx_state[channel].skip_counter = 0;
        player->sfx_state[channel].enabled = false;

        break;
    }

    player_update_status_register(player, false);

    return FAM_SUCCESS;
}