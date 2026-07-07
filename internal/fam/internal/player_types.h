#pragma once
#include <stdint.h>

#define MUSIC_NO_LOOP UINT64_MAX
#define SFX_CHANNEL_COUNT 4

typedef enum {
    CHAN_ID_PULSE1             = 0,
    CHAN_ID_PULSE2,
    CHAN_ID_TRIANGLE,
    CHAN_ID_NOISE,
    CHAN_ID_DMC,

    CHAN_COUNT,
} ChannelId;

typedef enum {
    CHAN_BIT_PULSE1             = 1 << CHAN_ID_PULSE1,
    CHAN_BIT_PULSE2             = 1 << CHAN_ID_PULSE2,
    CHAN_BIT_TRIANGLE           = 1 << CHAN_ID_TRIANGLE,
    CHAN_BIT_NOISE              = 1 << CHAN_ID_NOISE,
    CHAN_BIT_DMC                = 1 << CHAN_ID_DMC,
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

struct FamSfx {
    uint64_t channel_id;
    uint64_t stream_op_count;
    uint64_t stream_offset;
};