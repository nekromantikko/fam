#include <fam/player.h>
#include <fam/apu.h>
#include <fam/internal/stream_types.h>
#include <stdlib.h>
#include <string.h>

struct FamPlayer {
    FamApu* apu;
    uint32_t sample_rate;

    const FamMusic* music;
    uint32_t music_pos;
    uint8_t music_skip_counter;
    int8_t music_dpcm_sample_bank;
    bool music_paused;

    // Shadow state to track the music's "real" register state
    uint8_t reserve_pulse1[4];
    uint8_t reserve_pulse2[4];
    uint8_t reserve_triangle[4];
    uint8_t reserve_noise[4];
    uint8_t reserve_dmc[4];

    const FamSfx* sfx[SFX_CHANNEL_COUNT];
    uint32_t sfx_pos[SFX_CHANNEL_COUNT];
    uint8_t sfx_skip_counter[SFX_CHANNEL_COUNT];

    double accumulator;
    double cycle_counter;
};

static void player_clear_reserve(FamPlayer* player) {
    memset(player->reserve_pulse1, 0, 4);
    memset(player->reserve_pulse2, 0, 4);
    memset(player->reserve_triangle, 0, 4);
    memset(player->reserve_noise, 0, 4);
    memset(player->reserve_dmc, 0, 4);
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

static void player_update_status_register(FamPlayer* player) {
    uint8_t status = 0;

    if (player->music != NULL) {
        status |= player->music->channel_mask & 0x1F;
    }

    for (int i = 0; i < SFX_CHANNEL_COUNT; i++) {
        if (player->sfx[i] != NULL) {
            status |= 1 << i;
        }
    }

    fam_apu_write_register(player->apu, FAM_REGISTER_STATUS, status);
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
    if (player == NULL || player->music == NULL) {
        return 0;
    }

    if (player->music_dpcm_sample_bank < 0) {
        return 0;
    }

    DPCMSampleBank* bank = &player->music->dpcm_sample_banks[player->music_dpcm_sample_bank];

    uint32_t ind = addr - 0xC000;
    if (ind >= bank->size) {
        return 0;
    }

    return bank->data[ind];
}

static void player_process_music(FamPlayer* player) {
    if (player->music == NULL || player->music_paused) {
        return;
    }

    if (player->music_skip_counter > 0) {
        player->music_skip_counter--;
        return;
    }

    while (player->music_pos < player->music->stream_op_count) {
        StreamOperation op = player->music->stream[player->music_pos++];

        switch(op.opcode) {
            case OP_PULSE1_WRITE0:
            case OP_PULSE1_WRITE1:
            case OP_PULSE1_WRITE2:
            case OP_PULSE1_WRITE3:
                {
                    int offset = op.opcode - OP_PULSE1_WRITE0;
                    player->reserve_pulse1[offset] = op.data;
                    if (player->sfx[CHAN_ID_PULSE1] == NULL) {
                        fam_apu_write_register(player->apu, 0x4000 + offset, op.data);
                    }
                    break;
                }
            case OP_PULSE2_WRITE0:
            case OP_PULSE2_WRITE1:
            case OP_PULSE2_WRITE2:
            case OP_PULSE2_WRITE3:
                {
                    int offset = op.opcode - OP_PULSE2_WRITE0;
                    player->reserve_pulse2[offset] = op.data;
                    if (player->sfx[CHAN_ID_PULSE2] == NULL) {
                        fam_apu_write_register(player->apu, 0x4004 + offset, op.data);
                    }
                    break;
                }
            case OP_TRIANGLE_WRITE0:
            case OP_TRIANGLE_WRITE1:
            case OP_TRIANGLE_WRITE2:
            case OP_TRIANGLE_WRITE3:
                {
                    int offset = op.opcode - OP_TRIANGLE_WRITE0;
                    player->reserve_triangle[offset] = op.data;
                    if (player->sfx[CHAN_ID_TRIANGLE] == NULL) {
                        fam_apu_write_register(player->apu, 0x4008 + offset, op.data);
                    }
                    break;
                }
            case OP_NOISE_WRITE0:
            case OP_NOISE_WRITE1:
            case OP_NOISE_WRITE2:
            case OP_NOISE_WRITE3:
                {
                    int offset = op.opcode - OP_NOISE_WRITE0;
                    player->reserve_noise[offset] = op.data;
                    if (player->sfx[CHAN_ID_NOISE] == NULL) {
                        fam_apu_write_register(player->apu, 0x400C + offset, op.data);
                    }
                    break;
                }
            case OP_DMC_WRITE0:
            case OP_DMC_WRITE1:
            case OP_DMC_WRITE2:
            case OP_DMC_WRITE3:
                {
                    int offset = op.opcode - OP_DMC_WRITE0;
                    player->reserve_dmc[offset] = op.data;
                    fam_apu_write_register(player->apu, 0x4010 + offset, op.data);
                    break;
                }
            case OP_DMC_PLAY_SAMPLE:
                player_update_status_register(player);
                break;
            case OP_SWITCH_SAMPLE_BANK:
                player->music_dpcm_sample_bank = (int8_t)op.data;
                break;
            case OP_ENDFRAME:
                player->music_skip_counter = op.data;
                return;
            
            case OP_ENDSTREAM:
                goto endstream;

            default:
                break;
        }
    }

endstream:

    // End of song reached (Or loop point out of bounds)
    if (player->music->loop_point == MUSIC_NO_LOOP || player->music->loop_point >= player->music->stream_op_count) {
        fam_player_stop_music(player);
    } else {
        player->music_pos = player->music->loop_point;
    }
}

static void player_process_sfx(FamPlayer* player, int channel) {
    const FamSfx* sfx = player->sfx[channel];

    if (sfx == NULL) return;

    if (player->sfx_skip_counter[channel] > 0) {
        player->sfx_skip_counter[channel]--;
        return;
    }

    while (player->sfx_pos[channel] < sfx->stream_op_count) {
        StreamOperation op = sfx->stream[player->sfx_pos[channel]++];

        switch(op.opcode) {
            case OP_PULSE1_WRITE0:
            case OP_PULSE1_WRITE1:
            case OP_PULSE1_WRITE2:
            case OP_PULSE1_WRITE3:
                {
                    if (channel != CHAN_ID_PULSE1) break;

                    int offset = op.opcode - OP_PULSE1_WRITE0;
                    fam_apu_write_register(player->apu, 0x4000 + offset, op.data);
                    break;
                }
            case OP_PULSE2_WRITE0:
            case OP_PULSE2_WRITE1:
            case OP_PULSE2_WRITE2:
            case OP_PULSE2_WRITE3:
                {
                    if (channel != CHAN_ID_PULSE2) break;

                    int offset = op.opcode - OP_PULSE2_WRITE0;
                    fam_apu_write_register(player->apu, 0x4004 + offset, op.data);
                    break;
                }
            case OP_TRIANGLE_WRITE0:
            case OP_TRIANGLE_WRITE1:
            case OP_TRIANGLE_WRITE2:
            case OP_TRIANGLE_WRITE3:
                {
                    if (channel != CHAN_ID_TRIANGLE) break;

                    int offset = op.opcode - OP_TRIANGLE_WRITE0;
                    fam_apu_write_register(player->apu, 0x4008 + offset, op.data);
                    break;
                }
            case OP_NOISE_WRITE0:
            case OP_NOISE_WRITE1:
            case OP_NOISE_WRITE2:
            case OP_NOISE_WRITE3:
                {
                    if (channel != CHAN_ID_NOISE) break;

                    int offset = op.opcode - OP_NOISE_WRITE0;
                    fam_apu_write_register(player->apu, 0x400C + offset, op.data);
                    break;
                }
            case OP_DMC_WRITE0:
            case OP_DMC_WRITE1:
            case OP_DMC_WRITE2:
            case OP_DMC_WRITE3:
            case OP_DMC_PLAY_SAMPLE:
                break;

            case OP_ENDFRAME:
                player->sfx_skip_counter[channel] = op.data;
                return;
            
            case OP_ENDSTREAM:
                goto endstream;
            
            default:
                break;
        }
    }

endstream:

    player->sfx[channel] = NULL;
    player_update_status_register(player);
    player_restore_reserve(player, channel);

    // Re-mute paused music after reserve restore
    if (player->music_paused) {
        player_silence_music(player);
    }
}

static void player_process_frame(FamPlayer* player) {
    player_process_music(player);
    
    for (int i = 0; i < SFX_CHANNEL_COUNT; i++) {
        player_process_sfx(player, i);
    }
}

FamResult fam_player_init(FamPlayer** out_player, FamApu* apu, uint32_t sample_rate) {
    if (out_player == NULL || apu == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FamPlayer* player = (FamPlayer*)calloc(1, sizeof(FamPlayer));
    if (player == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    player->apu = apu;
    player->sample_rate = sample_rate;
    memset((void*)player->sfx, 0, sizeof(FamSfx*) * SFX_CHANNEL_COUNT);

    fam_apu_set_dmc_reader(apu, player_dmc_callback, player);
    // 4-step sequence, disable IRQ
    fam_apu_write_register(player->apu, FAM_REGISTER_FRAME_COUNTER, 0x40);

    // Init reserve state
    player_clear_reserve(player);

    *out_player = player;
    return FAM_SUCCESS;
}

void fam_player_free(FamPlayer* player) {
    free(player);
}

void fam_player_process_samples(FamPlayer* player, int sample_count, void* out_samples) {
    if (sample_count == 0) {
        return;
    }

    if (player == NULL || out_samples == NULL) {
        return;
    }

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

        // TODO: Other output formats
        float* sample = ((float*)out_samples) + i;
        // TODO: Average samples across multiple APU clocks to prevent aliasing
        fam_apu_get_sample(player->apu, sample);
    }
}

void fam_player_play_music(FamPlayer* player, const FamMusic* music) {
    if (player == NULL || music == NULL) {
        return;
    }

    player->music = music;
    player->music_pos = 0;
    player->music_skip_counter = 0;
    player->music_paused = false;
    player->music_dpcm_sample_bank = music->dpcm_sample_bank_count == 0 ? -1 : 0;

    // Reset reserve state
    player_clear_reserve(player);

    player_update_status_register(player);
}

void fam_player_pause_music(FamPlayer* player) {
    if (player == NULL) {
        return;
    }

    if (player->music == NULL || player->music_paused) {
        return;  
    }

    player->music_paused = true;
    player_silence_music(player);
}

void fam_player_resume_music(FamPlayer* player) {
    if (player == NULL) {
        return;
    }

    if (player->music == NULL || !player->music_paused) {
        return;
    }

    player->music_paused = false;

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

    player_update_status_register(player);
}

void fam_player_play_sfx(FamPlayer* player, const FamSfx* sfx) {
    if (player == NULL || sfx == NULL) {
        return;
    }

    if (sfx->channel_id >= SFX_CHANNEL_COUNT) {
        return;
    }

    player->sfx[sfx->channel_id] = sfx;
    player->sfx_pos[sfx->channel_id] = 0;
    player->sfx_skip_counter[sfx->channel_id] = 0;

    player_update_status_register(player);
}