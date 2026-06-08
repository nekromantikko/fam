#include <fam/player.h>
#include <fam/apu.h>
#include <stdlib.h>
#include <string.h>

#define NO_LOOP UINT64_MAX

#define ADDR_ENDFRAME 0x0000

#define RECORD_SIZE 3

struct FamMusic {
    uint64_t sample_data_size;
    uint64_t sample_data_offset;
    uint64_t record_count;
    uint64_t record_offset;
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

    const uint8_t* record_data = (uint8_t*)player->music + player->music->record_offset; 

    while (player->music_pos < player->music->record_count) {
        static uint8_t record[RECORD_SIZE];
        memcpy(record, record_data + player->music_pos * RECORD_SIZE, RECORD_SIZE);

        uint16_t addr = (uint16_t)record[0] |
                        ((uint16_t)record[1] << 8);
        uint8_t data = record[2];
        player->music_pos++;

        if (addr == ADDR_ENDFRAME) {
            player->music_skip_counter = data;
            return;
        }

        // TODO: Once there is music and SFX playing at the same time,
        // $4015 writes should be masked to not disable channels needed by other sounds
        // $4015 writes need to be allowed in order to play DMC samples

        // $4017 is owned by the player, ignore writes to it
        if (addr == FAM_REGISTER_FRAME_COUNTER) {
            continue;
        }
        fam_apu_write_register(player->apu, addr, data);
    }

    // End of song reached
    if (player->music->loop_point == NO_LOOP) {
        fam_player_stop_music(player);
    } else {
        // TODO: Validate that loop point is < record_count!
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

    // TODO: Channel mask for only enabling specific channels
    fam_apu_write_register(player->apu, FAM_REGISTER_STATUS, 0x1F);
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