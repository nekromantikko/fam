#include <fam/player.h>
#include <fam/apu.h>
#include <stdlib.h>
#include <string.h>

#define NO_LOOP UINT64_MAX

typedef enum {
    CHAN_PULSE1             = 1,
    CHAN_PULSE2             = 1 << 1,
    CHAN_TRIANGLE           = 1 << 2,
    CHAN_NOISE              = 1 << 3,
    CHAN_DMC                = 1 << 4,
} ChannelFlags;

typedef enum {
    OP_PULSE1_WRITE0        = 0x0,
    OP_PULSE1_WRITE1        = 0x1,
    OP_PULSE1_WRITE2        = 0x2,
    OP_PULSE1_WRITE3        = 0x3,

    OP_PULSE2_WRITE0        = 0x4,
    OP_PULSE2_WRITE1        = 0x5,
    OP_PULSE2_WRITE2        = 0x6,
    OP_PULSE2_WRITE3        = 0x7,

    OP_TRIANGLE_WRITE0      = 0x8,
    OP_TRIANGLE_WRITE1      = 0x9,
    OP_TRIANGLE_WRITE2      = 0xA,
    OP_TRIANGLE_WRITE3      = 0xB,

    OP_NOISE_WRITE0         = 0xC,
    OP_NOISE_WRITE1         = 0xD,
    OP_NOISE_WRITE2         = 0xE,
    OP_NOISE_WRITE3         = 0xF,

    OP_DMC_WRITE0           = 0x10,
    OP_DMC_WRITE1           = 0x11,
    OP_DMC_WRITE2           = 0x12,
    OP_DMC_WRITE3           = 0x13,

    OP_DMC_PLAY_SAMPLE      = 0x14,

    OP_ENDFRAME             = 0xFE,
    OP_ENDSTREAM            = 0xFF,
} StreamOpCode;

typedef struct StreamOperation {
    uint8_t opcode;
    uint8_t data;
} StreamOperation;

struct FamMusic {
    uint64_t channel_mask;
    uint64_t sample_data_size;
    uint64_t sample_data_offset;
    uint64_t stream_op_count;
    uint64_t stream_offset;
    uint64_t loop_point;
};

struct FamPlayer {
    FamApu* apu;
    uint32_t sample_rate;
    const FamMusic* music;
    size_t music_pos;
    uint8_t music_skip_counter;
    bool music_paused;

    double accumulator;
};

static uint8_t player_dmc_callback(void* user_data, uint16_t addr) {
    // TODO: "Bank switching" if we want more than 16KB of sample data
    FamPlayer* player = (FamPlayer*)user_data;
    if (player == NULL || player->music == NULL) {
        return 0;
    }

    size_t ind = addr - 0xC000;
    if (ind >= player->music->sample_data_size) {
        return 0;
    }

    const uint8_t* sample_data = (uint8_t*)player->music + player->music->sample_data_offset;
    return sample_data[ind];
}

static void player_process_frame(FamPlayer* player) {
    if (player->music == NULL || player->music_paused) {
        return;
    }

    if (player->music_skip_counter > 0) {
        player->music_skip_counter--;
        return;
    }

    const StreamOperation* stream_data = (StreamOperation*)((uint8_t*)player->music + player->music->stream_offset); 

    while (player->music_pos < player->music->stream_op_count) {
        StreamOperation op = stream_data[player->music_pos++];

        switch(op.opcode) {
            case OP_PULSE1_WRITE0:
                fam_apu_write_register(player->apu, 0x4000, op.data);
                break;
            case OP_PULSE1_WRITE1:
                fam_apu_write_register(player->apu, 0x4001, op.data);
                break;
            case OP_PULSE1_WRITE2:
                fam_apu_write_register(player->apu, 0x4002, op.data);
                break;
            case OP_PULSE1_WRITE3:
                fam_apu_write_register(player->apu, 0x4003, op.data);
                break;
            
            case OP_PULSE2_WRITE0:
                fam_apu_write_register(player->apu, 0x4004, op.data);
                break;
            case OP_PULSE2_WRITE1:
                fam_apu_write_register(player->apu, 0x4005, op.data);
                break;
            case OP_PULSE2_WRITE2:
                fam_apu_write_register(player->apu, 0x4006, op.data);
                break;
            case OP_PULSE2_WRITE3:
                fam_apu_write_register(player->apu, 0x4007, op.data);
                break;

            case OP_TRIANGLE_WRITE0:
                fam_apu_write_register(player->apu, 0x4008, op.data);
                break;
            case OP_TRIANGLE_WRITE1:
                fam_apu_write_register(player->apu, 0x4009, op.data);
                break;
            case OP_TRIANGLE_WRITE2:
                fam_apu_write_register(player->apu, 0x400A, op.data);
                break;
            case OP_TRIANGLE_WRITE3:
                fam_apu_write_register(player->apu, 0x400B, op.data);
                break;

            case OP_NOISE_WRITE0:
                fam_apu_write_register(player->apu, 0x400C, op.data);
                break;
            case OP_NOISE_WRITE1:
                fam_apu_write_register(player->apu, 0x400D, op.data);
                break;
            case OP_NOISE_WRITE2:
                fam_apu_write_register(player->apu, 0x400E, op.data);
                break;
            case OP_NOISE_WRITE3:
                fam_apu_write_register(player->apu, 0x400F, op.data);
                break;

            case OP_DMC_WRITE0:
                fam_apu_write_register(player->apu, 0x4010, op.data);
                break;
            case OP_DMC_WRITE1:
                fam_apu_write_register(player->apu, 0x4011, op.data);
                break;
            case OP_DMC_WRITE2:
                fam_apu_write_register(player->apu, 0x4012, op.data);
                break;
            case OP_DMC_WRITE3:
                fam_apu_write_register(player->apu, 0x4013, op.data);
                break;

            case OP_DMC_PLAY_SAMPLE:
                fam_apu_write_register(player->apu, FAM_REGISTER_STATUS, player->music->channel_mask & 0xFF);
                break;

            case OP_ENDFRAME:
                player->music_skip_counter = op.data;
                return;
            
            case OP_ENDSTREAM:
                goto endstream;
        }
    }

endstream:

    // End of song reached (Or loop point out of bounds)
    if (player->music->loop_point == NO_LOOP || player->music->loop_point >= player->music->stream_op_count) {
        fam_player_stop_music(player);
    } else {
        player->music_pos = player->music->loop_point;
    }
}

FamResult fam_player_init(uint32_t sample_rate, FamPlayer** out_player) {
    if (out_player == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FamApu* apu;
    FamResult err = fam_apu_init(&apu);
    if (err != FAM_SUCCESS) {
        return err;
    }

    FamPlayer* player = (FamPlayer*)calloc(1, sizeof(FamPlayer));
    if (player == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    player->apu = apu;
    player->sample_rate = sample_rate;

    fam_apu_set_dmc_reader(apu, player_dmc_callback, player);

    *out_player = player;
    return FAM_SUCCESS;
}

void fam_player_free(FamPlayer* player) {
    fam_apu_free(player->apu);
    free(player);
}

void fam_player_process_samples(FamPlayer* player, int count, void* out_samples) {
    // TODO: Cache these values?
    const double apu_period = 1.0 / fam_apu_get_freq(player->apu);
    const double sample_time = 1.0 / (double)player->sample_rate;

    for (int i = 0; i < count; i++) {
        player->accumulator += sample_time;
        while (player->accumulator >= apu_period) {
            fam_apu_clock(player->apu);
            player->accumulator -= apu_period;

            // Check frame interrupt flag and process frame
            static uint8_t status;
            fam_apu_read_register(player->apu, FAM_REGISTER_STATUS, &status);
            if (status & 0x40) {
                player_process_frame(player);
            }
        }

        // TODO: Other output formats
        float* sample = ((float*)out_samples) + i;
        fam_apu_get_sample(player->apu, sample);
    }
}

// TODO: Command buffer for thread safety
void fam_player_play_music(FamPlayer* player, const FamMusic* music) {
    player->music = music;
    player->music_pos = 0;
    player->music_skip_counter = 0;
    player->music_paused = false;

    fam_apu_write_register(player->apu, FAM_REGISTER_STATUS, music->channel_mask & 0xFF);
    fam_apu_write_register(player->apu, FAM_REGISTER_FRAME_COUNTER, 0x00);
}

void fam_player_pause_music(FamPlayer* player) {
    player->music_paused = true;

    fam_apu_write_register(player->apu, FAM_REGISTER_STATUS, 0x00);
}

void fam_player_resume_music(FamPlayer* player) {
    player->music_paused = false;

    fam_apu_write_register(player->apu, FAM_REGISTER_STATUS, 0x1F);
    fam_apu_write_register(player->apu, FAM_REGISTER_FRAME_COUNTER, 0x00);
}

void fam_player_stop_music(FamPlayer* player) {
    player->music = NULL;

    fam_apu_write_register(player->apu, FAM_REGISTER_STATUS, 0x00);
}