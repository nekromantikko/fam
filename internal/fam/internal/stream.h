#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Stream format specification:
// Multi-byte fields are little-endian, all offsets are absolute
// ZII, so that new fields can be added in subsequent versions without breaking backwards compatibility

//  Header (64 bytes):
//  0x00        char[4]     magic "FAM\0"
//  0x04        u16         version_major
//  0x06        u16         version_minor
//
//  0x08        u8          usage
//  0x09        u8          region
//  0x0A-0x0B   u16         music_dpcm_bank_count
//  0x0C-0x0F   u32         music_dpcm_bank_data_offset
//
//  0x10-0x13   u32         eof_offset (AKA size)
//  0x14-0x17   u32         music_loop_offset (0 means no loop)
//
//  0x18-0x1B   u32         op_count
//  0x1C-0x1F   u32         op_data_offset
//
//  0x20-0x27   u64         channel_mask
//
//  0x28-0x3F               reserved

// Stream data consists of 2-byte operations, where the first byte is the opcode and the second byte is data

#define STREAM_MAGIC "FAM"
#define STREAM_VERSION_MAJOR 1
#define STREAM_VERSION_MINOR 0

#define STREAM_HEADER_SIZE                                      0x40
#define STREAM_HEADER_FIELD_MAGIC                               0x00
#define STREAM_HEADER_FIELD_VERSION_MAJOR                       0x04
#define STREAM_HEADER_FIELD_VERSION_MINOR                       0x06
#define STREAM_HEADER_FIELD_USAGE                               0x08
#define STREAM_HEADER_FIELD_REGION                              0x09
#define STREAM_HEADER_FIELD_MUSIC_DPCM_BANK_COUNT               0x0A
#define STREAM_HEADER_FIELD_MUSIC_DPCM_BANK_DATA_OFFSET         0x0C
#define STREAM_HEADER_FIELD_EOF_OFFSET                          0x10
#define STREAM_HEADER_FIELD_MUSIC_LOOP_OFFSET                   0x14
#define STREAM_HEADER_FIELD_OP_COUNT                            0x18
#define STREAM_HEADER_FIELD_OP_DATA_OFFSET                      0x1C
#define STREAM_HEADER_FIELD_CHANNEL_MASK                        0x20

#define STREAM_OP_SIZE 2
#define MUSIC_DPCM_SAMPLE_BANK_SIZE 0x4000 // 16 kB
#define MUSIC_MAX_DPCM_BANK_COUNT 256 // Needs to be addressable with one byte

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

#define CHANNEL_MASK_SUPPORTED ((1ull << CHAN_COUNT) - 1)

#define SFX_CHANNEL_COUNT 4
#define CHANNEL_MASK_SFX ((1ull << SFX_CHANNEL_COUNT) - 1)

// The channels controlled by the APU status register $4015
#define CHANNEL_MASK_APU_STATUS ((1ull << (CHAN_ID_DMC + 1)) - 1)

// Opcode allocation policy for expansion chips:
//
// One opcode per register will not fit all the planned expansion chips.
// The six planned chips have 325 addressable registers between them,
// and we only have 232 to spare (0x16-0xFD).
// Therefore, opcodes should be assigned according to the following rules:
//
// 1. Per-frame register traffic gets direct opcodes for chips with a small
//    register file. Cheapest in file size, and this is the traffic that repeats every frame.
// 2. Chips with a hardware address/data port pair get an ADDR + DATA opcode pair when their
//    register file is large. Namco 163's data port auto-increments, so burst writes are actually
//    cheaper this way than with direct opcodes.
// 3. Static tables (wave RAM, modulation tables) live in data sections addressed by an opcode,
//    same way the DPCM banks work right now.
// 4. Opcode blocks are allocated per chip in implementation order.

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

    OP_SWITCH_SAMPLE_BANK   = 0x14,
    OP_STATUS_WRITE         = 0x15,

    OP_ENDFRAME             = 0xFE,
    OP_ENDSTREAM            = 0xFF,
} StreamOpCode;

typedef enum {
    STREAM_USAGE_MUSIC = 0,
    STREAM_USAGE_SFX,

    STREAM_USAGE_COUNT
} StreamUsage;

typedef struct StreamInfo {
    uint8_t usage;
    uint8_t region;
    uint16_t music_dpcm_bank_count;
    uint32_t music_dpcm_bank_data_offset;
    size_t music_dpcm_bank_data_size; // Calculated helper field

    size_t total_size;
    uint32_t music_loop_offset; // 0 means no loop

    uint32_t op_count;
    uint32_t op_data_offset;
    size_t op_data_size; // Calculated helper field

    uint64_t channel_mask;
} StreamInfo;

// TODO: Move to utils?
static inline uint16_t read_u16_le(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t read_u64_le(const uint8_t* p) {
    return (uint64_t)read_u32_le(p) | (uint64_t)read_u32_le(p + 4) << 32;
}

static inline const StreamInfo stream_get_info(const uint8_t* buffer) {
    const uint16_t bank_count = read_u16_le(buffer + STREAM_HEADER_FIELD_MUSIC_DPCM_BANK_COUNT);
    const uint32_t op_count = read_u32_le(buffer + STREAM_HEADER_FIELD_OP_COUNT);
    
    const StreamInfo info = {
        .usage = buffer[STREAM_HEADER_FIELD_USAGE],
        .region = buffer[STREAM_HEADER_FIELD_REGION],
        .music_dpcm_bank_count = bank_count,
        .music_dpcm_bank_data_offset = read_u32_le(buffer + STREAM_HEADER_FIELD_MUSIC_DPCM_BANK_DATA_OFFSET),
        .music_dpcm_bank_data_size = (size_t)bank_count * MUSIC_DPCM_SAMPLE_BANK_SIZE,
        .total_size = (size_t)read_u32_le(buffer + STREAM_HEADER_FIELD_EOF_OFFSET),
        .music_loop_offset = read_u32_le(buffer + STREAM_HEADER_FIELD_MUSIC_LOOP_OFFSET),
        .op_count = op_count,
        .op_data_offset = read_u32_le(buffer + STREAM_HEADER_FIELD_OP_DATA_OFFSET),
        .op_data_size = (size_t)op_count * STREAM_OP_SIZE,
        .channel_mask = read_u64_le(buffer + STREAM_HEADER_FIELD_CHANNEL_MASK)
    };

    return info;
}