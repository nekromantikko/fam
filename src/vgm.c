#include <fam/vgm.h>
#include <fam/internal/stream_types.h>
#include <fam/internal/buffer_reader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VGM_MAGIC "Vgm "
#define VGM_MIN_VERSION 0x00000161  // NES APU support was added in v1.61

// NES CPU clocks; the VGM clock field tells us which console the dump came from.
#define VGM_NES_CLOCK_NTSC 1789772
#define VGM_NES_CLOCK_PAL  1662607

// Header field offsets
#define VGM_OFFSET_VERSION   0x08
#define VGM_OFFSET_LOOP      0x1C   // relative to this field; 0 = no loop
#define VGM_OFFSET_DATA      0x34   // relative to this field; 0 = default 0x40
#define VGM_OFFSET_NES_CLOCK 0x84

// VGM timing is a fixed 44100 Hz sample clock; one frame is that many samples. NTSC music
// updates at 60 Hz (which is also our fam playback rate), PAL at 50 Hz.
#define VGM_SAMPLES_PER_FRAME_NTSC 735  // 44100 / 60
#define VGM_SAMPLES_PER_FRAME_PAL  882  // 44100 / 50

// Stream commands we care about (a NES-only rip is writes + waits + end + data blocks)
#define VGM_CMD_WAIT_N       0x61   // followed by uint16 sample count
#define VGM_CMD_WAIT_FRAME   0x62   // wait one NTSC frame (735 samples, 1/60 s)
#define VGM_CMD_WAIT_FRAME_PAL 0x63 // wait one PAL frame (882 samples, 1/50 s)
#define VGM_CMD_END          0x66
#define VGM_CMD_DATA_BLOCK   0x67
#define VGM_CMD_NES_APU      0xB4   // followed by register offset + value
// 0x70..0x7F: wait (low nibble + 1) samples

#define VGM_DATA_BLOCK_NES_DPCM 0xC2  // 0x67 data block type: NES APU DPCM sample RAM write

typedef struct {
    uint32_t version;
    uint32_t data_offset;   // absolute
    uint32_t loop_offset;   // absolute, 0 = no loop
    uint32_t nes_clock;
} VgmHeader;

// The DMC reads samples from the $C000-$FFFF window, which on hardware is banked ROM. VGM
// re-dumps that window (a 0xC2 block) every time the original game switched banks. We treat
// each distinct window state as a bank; a "generation" is the window between two 0xC2 blocks.
// A bank IS the DMC window, so MAX_DPCM_SAMPLE_BANK_SIZE is exactly the window size (16 KB).
typedef struct {
    uint8_t* window;        // 16 KB snapshot, used for dedup; trimmed to max_extent when stored
    uint32_t max_extent;    // bank size: the most bytes any generation reads from this window
} DpcmBank;

// Single-pass conversion state: emit the op stream while reconstructing the DPCM banks on the
// fly. Banks are deduped in first-appearance order, so switches can be emitted live.
typedef struct {
    StreamOperation* stream_start;
    StreamOperation* stream_pos;
    uint64_t channel_mask;
    uint32_t loop_point;
    uint32_t sample_accum;      // VGM samples of wait not yet flushed to frames
    uint32_t samples_per_frame; // NTSC or PAL

    uint8_t* window;            // 16 KB scratch: the running $C000-$FFFF window
    DpcmBank banks[MAX_DPCM_BANK_COUNT];
    uint32_t bank_count;
    int32_t cur_bank;           // active bank (last OP_SWITCH target); -1 = none yet
    uint32_t used_max;          // bytes read from the current window generation
    uint8_t cur_sample_addr;    // last $4012 write
    uint8_t cur_sample_len;     // last $4013 write
} VgmConverter;

static FamResult read_u32_at(BufferReader* reader, size_t offset, uint32_t* out) {
    buffer_reader_seek(reader, offset);
    buffer_reader_read(reader, out, sizeof(uint32_t));
    return reader->error ? FAM_ERROR_INVALID_FORMAT : FAM_SUCCESS;
}

static FamResult parse_header(BufferReader* reader, VgmHeader* out) {
    uint32_t magic;
    buffer_reader_read(reader, &magic, sizeof(uint32_t));
    if (reader->error || memcmp(&magic, VGM_MAGIC, 4) != 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if (read_u32_at(reader, VGM_OFFSET_VERSION, &out->version) != FAM_SUCCESS ||
        out->version < VGM_MIN_VERSION) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint32_t rel_loop;
    if (read_u32_at(reader, VGM_OFFSET_LOOP, &rel_loop) != FAM_SUCCESS) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    out->loop_offset = (rel_loop != 0) ? VGM_OFFSET_LOOP + rel_loop : 0;

    uint32_t rel_data;
    if (read_u32_at(reader, VGM_OFFSET_DATA, &rel_data) != FAM_SUCCESS) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    out->data_offset = (rel_data != 0) ? VGM_OFFSET_DATA + rel_data : 0x40;

    // Bit 31 flags a second (dual) chip; mask it off. A zero clock means no NES APU.
    if (read_u32_at(reader, VGM_OFFSET_NES_CLOCK, &out->nes_clock) != FAM_SUCCESS ||
        (out->nes_clock & 0x7FFFFFFF) == 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    printf("VGM v%x, data @ 0x%x, loop @ 0x%x, NES clock %u\n",
        out->version, out->data_offset, out->loop_offset, out->nes_clock & 0x7FFFFFFF);

    return FAM_SUCCESS;
}

// Picks the machine whose CPU clock the dump's NES clock is closest to.
// TODO: Fail if it's too far away from either?
static uint8_t machine_from_nes_clock(uint32_t clock) {
    clock &= 0x7FFFFFFF; // bit 31 flags a dual chip
    uint32_t d_ntsc = (clock > VGM_NES_CLOCK_NTSC) ? clock - VGM_NES_CLOCK_NTSC : VGM_NES_CLOCK_NTSC - clock;
    uint32_t d_pal  = (clock > VGM_NES_CLOCK_PAL)  ? clock - VGM_NES_CLOCK_PAL  : VGM_NES_CLOCK_PAL - clock;
    return (d_pal < d_ntsc) ? FAM_MACHINE_PAL : FAM_MACHINE_NTSC;
}

static void converter_free(VgmConverter* c) {
    free(c->window);
    free(c->stream_start);
    for (uint32_t i = 0; i < c->bank_count; i++) {
        free(c->banks[i].window);
    }
}

static void converter_emit(VgmConverter* c, uint8_t opcode, uint8_t data) {
    *c->stream_pos++ = (StreamOperation){ opcode, data };
}

// A wait of `frames` frames, chunked into OP_ENDFRAMEs (each spans up to OP_ENDFRAME_MAX_SPAN).
static void converter_emit_wait(VgmConverter* c, uint32_t frames) {
    while (frames > 0) {
        uint32_t chunk = (frames > OP_ENDFRAME_MAX_SPAN) ? OP_ENDFRAME_MAX_SPAN : frames;
        converter_emit(c, OP_ENDFRAME, (uint8_t)(chunk - 1));
        frames -= chunk;
    }
}

// Returns the index of the bank holding the current window, adding one if it's new.
// -1 = too many banks, -2 = out of memory.
static int32_t converter_bank_for_window(VgmConverter* c) {
    for (uint32_t i = 0; i < c->bank_count; i++) {
        if (memcmp(c->banks[i].window, c->window, MAX_DPCM_SAMPLE_BANK_SIZE) == 0) {
            return (int32_t)i;
        }
    }
    if (c->bank_count >= MAX_DPCM_BANK_COUNT) {
        return -1;
    }
    uint8_t* copy = (uint8_t*)malloc(MAX_DPCM_SAMPLE_BANK_SIZE);
    if (copy == NULL) {
        return -2;
    }
    memcpy(copy, c->window, MAX_DPCM_SAMPLE_BANK_SIZE);
    c->banks[c->bank_count].window = copy;
    c->banks[c->bank_count].max_extent = 0;
    return (int32_t)c->bank_count++;
}

// Walk the VGM command stream once: emit the op stream, quantize waits into whole frames, and
// reconstruct the DPCM banks (each distinct $C000 window becomes a bank, switched live).
static FamResult convert_commands(BufferReader* reader, const VgmHeader* header, VgmConverter* c) {
    buffer_reader_seek(reader, header->data_offset);
    bool done = false;

    while (!done) {
        // The loop target always lands on a command boundary; record which op it maps to.
        if (header->loop_offset != 0 && c->loop_point == MUSIC_NO_LOOP &&
            buffer_reader_tell(reader) == header->loop_offset) {
            c->loop_point = (uint32_t)(c->stream_pos - c->stream_start);
        }

        uint8_t cmd;
        buffer_reader_read(reader, &cmd, 1);
        if (reader->error) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        switch (cmd) {
            case VGM_CMD_NES_APU: {
                uint8_t reg, value;
                buffer_reader_read(reader, &reg, 1);
                buffer_reader_read(reader, &value, 1);
                // reg is the offset from $4000, which is exactly our opcode numbering for the
                // channel register writes: $4000-$4013 -> OP_PULSE1_WRITE0 .. OP_DMC_WRITE3.
                if (reg <= OP_DMC_WRITE3) {
                    converter_emit(c, reg, value);
                    c->channel_mask |= (uint64_t)1 << (reg >> 2);
                    if (reg == 0x12) {
                        c->cur_sample_addr = value;
                    } else if (reg == 0x13) {
                        c->cur_sample_len = value;
                    }
                } else if (reg == 0x15) {
                    // $4015 channel enables -> OP_STATUS_WRITE. Emit every write, not just
                    // changes: a repeated value can be a DMC re-trigger. The channel mask
                    // must cover every channel a status write is allowed to enable.
                    if (value & CHAN_BIT_DMC) {
                        // A DMC trigger: grow the current generation's read extent.
                        uint32_t extent = ((uint32_t)c->cur_sample_addr << 6)
                                        + ((uint32_t)c->cur_sample_len << 4) + 1;
                        if (extent > MAX_DPCM_SAMPLE_BANK_SIZE) {
                            extent = MAX_DPCM_SAMPLE_BANK_SIZE;
                        }
                        if (extent > c->used_max) {
                            c->used_max = extent;
                        }
                    }
                    converter_emit(c, OP_STATUS_WRITE, value & 0x1F);
                    c->channel_mask |= value & 0x1F;
                }
                // $4017 (frame counter) and $4014 (OAM DMA, non-audio) are ignored.
                break;
            }
            case VGM_CMD_WAIT_N: {
                uint16_t samples;
                buffer_reader_read(reader, &samples, sizeof(uint16_t));
                c->sample_accum += samples;
                break;
            }
            case VGM_CMD_WAIT_FRAME:
                c->sample_accum += VGM_SAMPLES_PER_FRAME_NTSC;
                break;
            case VGM_CMD_WAIT_FRAME_PAL:
                c->sample_accum += VGM_SAMPLES_PER_FRAME_PAL;
                break;
            case VGM_CMD_END:
                done = true;
                break;
            case VGM_CMD_DATA_BLOCK: {
                // 0x67 0x66 tt ss(4) <ss bytes>. Type 0xC2 is a NES APU DPCM RAM write, whose
                // payload is <start_addr:2> <data>. Each such block ends the current generation
                // and starts a new one; other block types just skip the payload.
                uint8_t compat, type;
                uint32_t size;
                buffer_reader_read(reader, &compat, 1);
                buffer_reader_read(reader, &type, 1);
                buffer_reader_read(reader, &size, sizeof(uint32_t));

                if (type == VGM_DATA_BLOCK_NES_DPCM && size >= 2) {
                    // The outgoing generation ends here: commit its reads to its bank.
                    if (c->cur_bank >= 0 && c->used_max > c->banks[c->cur_bank].max_extent) {
                        c->banks[c->cur_bank].max_extent = c->used_max;
                    }
                    c->used_max = 0;

                    uint16_t start_addr;
                    buffer_reader_read(reader, &start_addr, sizeof(uint16_t));
                    uint32_t data_size = size - 2;
                    if (start_addr >= 0xC000) {
                        uint32_t offset = (uint32_t)(start_addr - 0xC000);
                        uint32_t n = (offset + data_size > MAX_DPCM_SAMPLE_BANK_SIZE)
                                   ? MAX_DPCM_SAMPLE_BANK_SIZE - offset : data_size;
                        buffer_reader_read(reader, c->window + offset, n);
                        buffer_reader_skip(reader, data_size - n); // drop any overflow tail
                    } else {
                        buffer_reader_skip(reader, data_size);
                    }

                    int32_t bank = converter_bank_for_window(c);
                    if (bank < 0) {
                        return (bank == -1) ? FAM_ERROR_UNSUPPORTED_FEATURE : FAM_ERROR_OUT_OF_MEMORY;
                    }
                    if (bank != c->cur_bank) {
                        converter_emit(c, OP_SWITCH_SAMPLE_BANK, (uint8_t)bank);
                        c->cur_bank = bank;
                    }
                } else {
                    buffer_reader_skip(reader, size);
                }
                break;
            }
            // DAC stream control (0x90-0x95) + PCM bank seek (0xE0). Trackers like Furnace use
            // these to play samples as PCM through a chip register (e.g. NES $4011). We don't
            // expand DAC streams into register writes yet, so we skip them at their fixed operand
            // lengths: a stream that's only set up (no 0x93/0x95 start) loses nothing, and an
            // actually-played stream is silent while the rest of the track plays.
            case 0x90: buffer_reader_skip(reader, 4); break;  // setup stream control
            case 0x91: buffer_reader_skip(reader, 4); break;  // set stream data
            case 0x92: buffer_reader_skip(reader, 5); break;  // set stream frequency
            case 0x93: buffer_reader_skip(reader, 10); break; // start stream
            case 0x94: buffer_reader_skip(reader, 1); break;  // stop stream
            case 0x95: buffer_reader_skip(reader, 4); break;  // start stream (fast)
            case 0xE0: buffer_reader_skip(reader, 4); break;  // seek PCM data bank

            default:
                if (cmd >= 0x70 && cmd <= 0x7F) {
                    c->sample_accum += (cmd & 0x0F) + 1; // wait 1..16 samples
                } else {
                    // Unknown command: we can't know its operand length, so bail cleanly
                    // rather than desync. Extend the handled set as needed.
                    return FAM_ERROR_UNSUPPORTED_FEATURE;
                }
                break;
        }
        if (reader->error) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        // Flush any whole frames the accumulated waits have crossed.
        if (c->sample_accum >= c->samples_per_frame) {
            uint32_t frames = c->sample_accum / c->samples_per_frame;
            c->sample_accum -= frames * c->samples_per_frame;
            converter_emit_wait(c, frames);
        }
    }

    // The final generation ends at the stream end.
    if (c->cur_bank >= 0 && c->used_max > c->banks[c->cur_bank].max_extent) {
        c->banks[c->cur_bank].max_extent = c->used_max;
    }

    converter_emit(c, OP_ENDSTREAM, 0);
    return FAM_SUCCESS;
}

FamResult fam_music_from_vgm_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer) {
    if (out_music == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    BufferReader header_reader = buffer_reader_init(buffer, buffer_size);
    VgmHeader header;
    FamResult result = parse_header(&header_reader, &header);
    if (result != FAM_SUCCESS) {
        return result;
    }

    if (header.data_offset >= buffer_size ||
        header.loop_offset >= buffer_size) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    uint8_t machine = machine_from_nes_clock(header.nes_clock);

    VgmConverter conv = {0};
    conv.loop_point = MUSIC_NO_LOOP;
    conv.cur_bank = -1;
    conv.samples_per_frame = machine == FAM_MACHINE_NTSC ? VGM_SAMPLES_PER_FRAME_NTSC : VGM_SAMPLES_PER_FRAME_PAL;
    conv.window = (uint8_t*)calloc(MAX_DPCM_SAMPLE_BANK_SIZE, 1);
    if (conv.window == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    // Upper bound on emitted ops: every command consumes at least one input byte and emits at
    // most one op, so the op count can never exceed the command stream's byte count. The
    // terminating 0x66 consumes a byte without emitting, which is exactly the slot OP_ENDSTREAM
    // needs, making the bound tight; the +1 is slack against a future command that emits two ops
    // for one command. Sized once here so the emit path needs no per-op bounds check.
    size_t stream_max_ops = (buffer_size - header.data_offset) + 1;

    conv.stream_start = (StreamOperation*)malloc(stream_max_ops * sizeof(StreamOperation));
    if (conv.stream_start == NULL) {
        converter_free(&conv);
        return FAM_ERROR_OUT_OF_MEMORY;
    }
    conv.stream_pos = conv.stream_start;

    BufferReader reader = buffer_reader_init(buffer, buffer_size);
    result = convert_commands(&reader, &header, &conv);
    if (result != FAM_SUCCESS) {
        converter_free(&conv);
        return result;
    }

    const uint32_t op_count = (uint32_t)(conv.stream_pos - conv.stream_start);
    const uint32_t bank_count = conv.bank_count;
    size_t bank_data_total = 0;
    for (uint32_t i = 0; i < bank_count; i++) {
        bank_data_total += conv.banks[i].max_extent;
    }

    printf("VGM: %u stream ops, loop point %u, channel mask %llu, %u banks, %zu DPCM bytes\n",
        op_count, conv.loop_point, (unsigned long long)conv.channel_mask, bank_count, bank_data_total);

    // Single contiguous block: struct + bank array + bank data + stream (mirrors io.c).
    // NOTE: sizeof(FamMusic) must be a multiple of alignof(DPCMSampleBank); holds while it is a
    // multiple of 8. Bank data and the stream are byte-aligned, so they can follow freely.
    size_t memory_size = sizeof(FamMusic)
        + (size_t)bank_count * sizeof(DPCMSampleBank)
        + bank_data_total
        + (size_t)op_count * sizeof(StreamOperation);
    void* memory = malloc(memory_size);
    if (memory == NULL) {
        converter_free(&conv);
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    FamMusic* music = (FamMusic*)memory;
    music->channel_mask = conv.channel_mask;
    music->dpcm_sample_bank_count = bank_count;
    music->dpcm_sample_banks = NULL;
    music->stream_op_count = op_count;
    music->stream = NULL;
    music->loop_point = (conv.loop_point != MUSIC_NO_LOOP && conv.loop_point < op_count) ? conv.loop_point : MUSIC_NO_LOOP;
    music->machine = machine;

    uint8_t* mem_pos = (uint8_t*)memory + sizeof(FamMusic);

    if (bank_count > 0) {
        music->dpcm_sample_banks = (DPCMSampleBank*)mem_pos;
        mem_pos += sizeof(DPCMSampleBank) * bank_count;

        for (uint32_t i = 0; i < bank_count; i++) {
            uint32_t sz = conv.banks[i].max_extent;
            music->dpcm_sample_banks[i].size = sz;
            music->dpcm_sample_banks[i].data = (sz > 0) ? mem_pos : NULL;
            if (sz > 0) {
                memcpy(mem_pos, conv.banks[i].window, sz);
                mem_pos += sz;
            }
        }
    }

    if (op_count > 0) {
        music->stream = (StreamOperation*)mem_pos;
        memcpy(mem_pos, conv.stream_start, (size_t)op_count * sizeof(StreamOperation));
    }

    converter_free(&conv);

    *out_music = music;
    return FAM_SUCCESS;
}

FamResult fam_music_from_vgm_file(FamMusic** out_music, const char* fname) {
    if (out_music == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FILE* file = fopen(fname, "rb");
    if (file == NULL) {
        return FAM_ERROR_IO;
    }

    fseek(file, 0, SEEK_END);
    long file_length = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_length < 0) {
        fclose(file);
        return FAM_ERROR_IO;
    }

    uint8_t* buffer = (uint8_t*)malloc((size_t)file_length);
    if (buffer == NULL) {
        fclose(file);
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    size_t read_len = fread(buffer, 1, (size_t)file_length, file);
    fclose(file);

    if (read_len != (size_t)file_length) {
        free(buffer);
        return FAM_ERROR_IO;
    }

    FamResult result = fam_music_from_vgm_buffer(out_music, (size_t)file_length, buffer);
    free(buffer);

    return result;
}
