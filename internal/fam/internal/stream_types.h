#pragma once

#define MUSIC_NO_LOOP UINT32_MAX
#define SFX_CHANNEL_COUNT 4
#define MAX_DPCM_SAMPLE_BANK_SIZE 0x4000 // 16 kB
#define MAX_DPCM_BANK_COUNT 128
#define MAX_STREAM_LENGTH UINT32_MAX

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
    OP_SWITCH_SAMPLE_BANK   = 0x15,

    OP_ENDFRAME             = 0xFE,
    OP_ENDSTREAM            = 0xFF,
} StreamOpCode;

typedef struct StreamOperation {
    uint8_t opcode;
    uint8_t data;
} StreamOperation;

typedef struct DPCMSampleBank {
    uint32_t size;
    uint8_t* data;
} DPCMSampleBank;

struct FamMusic {
    uint64_t channel_mask;
    uint32_t dpcm_sample_bank_count;
    DPCMSampleBank* dpcm_sample_banks;
    uint32_t stream_op_count;
    StreamOperation* stream;
    uint32_t loop_point;
};

struct FamSfx {
    uint8_t channel_id;
    uint32_t stream_op_count;
    StreamOperation* stream;
};