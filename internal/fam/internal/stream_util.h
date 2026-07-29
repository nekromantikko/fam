#pragma once
#include <fam/common.h>
#include <fam/internal/stream_types.h>
#include <stdlib.h>

static inline FamResult music_init(FamMusic** out_music, uint64_t channel_mask, uint32_t loop_point, uint32_t bank_count, const uint32_t* bank_sizes, uint32_t op_count) {
    if (out_music == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    if (bank_count > MAX_DPCM_BANK_COUNT ||
        bank_count > SIZE_MAX / sizeof(DPCMSampleBank)) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    if (op_count > MAX_STREAM_LENGTH ||
        op_count > SIZE_MAX / sizeof(StreamOperation)) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    // NOTE: sizeof(FamMusic) must be a multiple of alignof(DPCMSampleBank) (holds while it's a multiple of 8)
    // Bank data and the stream are byte-aligned, so they can follow freely
    size_t memory_size = sizeof(FamMusic)
        + (size_t)bank_count * sizeof(DPCMSampleBank)
        + (size_t)op_count * sizeof(StreamOperation);

    for (uint32_t i = 0; i < bank_count; i++) {
        if (bank_sizes[i] > MAX_DPCM_SAMPLE_BANK_SIZE ||
            bank_sizes[i] > SIZE_MAX - memory_size) {
            return FAM_ERROR_INVALID_ARGUMENT;
        }
        memory_size += bank_sizes[i];
    }

    FamMusic* music = (FamMusic*)malloc(memory_size);
    if (music == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    music->channel_mask = channel_mask;
    music->dpcm_sample_bank_count = bank_count;
    music->dpcm_sample_banks = NULL;
    music->stream_op_count = op_count;
    music->stream = NULL;
    music->loop_point = loop_point;

    uint8_t* mem_pos = (uint8_t*)music + sizeof(FamMusic);

    if (bank_count > 0) {
        music->dpcm_sample_banks = (DPCMSampleBank*)mem_pos;
        mem_pos += sizeof(DPCMSampleBank) * bank_count;

        for (uint32_t i = 0; i < bank_count; i++) {
            DPCMSampleBank* bank = &music->dpcm_sample_banks[i];
            bank->size = bank_sizes[i];
            bank->data = (bank_sizes[i] == 0) ? NULL : mem_pos;
            mem_pos += bank_sizes[i];
        }
    }

    if (op_count > 0) {
        music->stream = (StreamOperation*)mem_pos;
    }

    *out_music = music;
    return FAM_SUCCESS;
}