#pragma once
#include <stdint.h>

struct FamMusic {
    uint64_t channel_mask;
    uint64_t sample_data_size;
    uint64_t sample_data_offset;
    uint64_t stream_op_count;
    uint64_t stream_offset;
    uint64_t loop_point;
};

struct FamSfx {
    uint64_t channel_id;
    uint64_t stream_op_count;
    uint64_t stream_offset;
};