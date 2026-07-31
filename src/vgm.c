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

// Collects the converted op stream. When `stream` is NULL we only count (pass 1);
// otherwise we write (pass 2). Both passes are deterministic so the counts match.
typedef struct {
    StreamOperation* stream;
    uint32_t count;
    uint64_t channel_mask;
    uint8_t* dpcm;       // MAX_DPCM_SAMPLE_BANK_SIZE scratch for the $C000-$FFFF sample RAM,
                         // or NULL to skip bank building on this pass
    uint32_t dpcm_size;  // bytes actually populated (highest write extent)
} StreamWriter;

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

static FamResult emit(StreamWriter* w, uint8_t opcode, uint8_t data) {
    if (w->count >= MAX_STREAM_LENGTH) {
        return FAM_ERROR_INVALID_FORMAT; // stream too long
    }
    if (w->stream != NULL) {
        w->stream[w->count].opcode = opcode;
        w->stream[w->count].data = data;
    }
    w->count++;
    return FAM_SUCCESS;
}

// A wait of `frames` 50/60 Hz frames, chunked into OP_ENDFRAMEs (each spans up to
// OP_ENDFRAME_MAX_SPAN frames).
static FamResult emit_wait(StreamWriter* w, uint32_t frames) {
    while (frames > 0) {
        uint32_t chunk = (frames > OP_ENDFRAME_MAX_SPAN) ? OP_ENDFRAME_MAX_SPAN : frames;
        FamResult result = emit(w, OP_ENDFRAME, (uint8_t)(chunk - 1));
        if (result != FAM_SUCCESS) {
            return result;
        }
        frames -= chunk;
    }
    return FAM_SUCCESS;
}

// Walks the VGM command stream from the data offset, translating NES APU register writes
// into stream ops and quantizing the sample-clock waits into whole 50/60 Hz frames.
static FamResult walk_commands(BufferReader* reader, const VgmHeader* hdr, StreamWriter* w,
                               uint32_t* out_loop_point, uint32_t samples_per_frame) {
    buffer_reader_seek(reader, hdr->data_offset);

    uint32_t loop_point = MUSIC_NO_LOOP;
    uint32_t sample_accum = 0;
    bool done = false;

    while (!done) {
        // The loop target always lands on a command boundary; record which op it maps to.
        if (hdr->loop_offset != 0 && loop_point == MUSIC_NO_LOOP &&
            buffer_reader_tell(reader) == hdr->loop_offset) {
            loop_point = w->count;
        }

        uint8_t cmd;
        buffer_reader_read(reader, &cmd, 1);
        if (reader->error) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        FamResult result = FAM_SUCCESS;
        switch (cmd) {
            case VGM_CMD_NES_APU: {
                uint8_t reg, value;
                buffer_reader_read(reader, &reg, 1);
                buffer_reader_read(reader, &value, 1);
                // reg is the offset from $4000, which is exactly our opcode numbering for the
                // channel register writes: $4000-$4013 -> OP_PULSE1_WRITE0 .. OP_DMC_WRITE3.
                if (reg <= OP_DMC_WRITE3) {
                    result = emit(w, reg, value);
                    w->channel_mask |= (uint64_t)1 << (reg >> 2);
                } else if (reg == 0x15) {
                    // $4015 channel enables -> OP_STATUS_WRITE. Emit every write, not just
                    // changes: a repeated value can be a DMC re-trigger. The channel mask
                    // must cover every channel a status write is allowed to enable.
                    result = emit(w, OP_STATUS_WRITE, value & 0x1F);
                    w->channel_mask |= value & 0x1F;
                }
                // $4017 (frame counter) and $4014 (OAM DMA, non-audio) are ignored.
                break;
            }
            case VGM_CMD_WAIT_N: {
                uint16_t samples;
                buffer_reader_read(reader, &samples, sizeof(uint16_t));
                sample_accum += samples;
                break;
            }
            case VGM_CMD_WAIT_FRAME:
                sample_accum += VGM_SAMPLES_PER_FRAME_NTSC;
                break;
            case VGM_CMD_WAIT_FRAME_PAL:
                sample_accum += VGM_SAMPLES_PER_FRAME_PAL;
                break;
            case VGM_CMD_END:
                done = true;
                break;
            case VGM_CMD_DATA_BLOCK: {
                // 0x67 0x66 tt ss(4) <ss bytes>. Type 0xC2 is a NES APU DPCM RAM write, whose
                // payload is <start_addr:2> <data>; copy that into the sample bank. Other block
                // types (and pass 2, where dpcm is NULL) just skip the payload.
                uint8_t compat, type;
                uint32_t size;
                buffer_reader_read(reader, &compat, 1);
                buffer_reader_read(reader, &type, 1);
                buffer_reader_read(reader, &size, sizeof(uint32_t));

                if (type == VGM_DATA_BLOCK_NES_DPCM && size >= 2 && w->dpcm != NULL) {
                    uint16_t start_addr;
                    buffer_reader_read(reader, &start_addr, sizeof(uint16_t));
                    uint32_t data_size = size - 2;
                    // The DPCM sample space is $C000-$FFFF, which the bank represents 0-based.
                    if (start_addr >= 0xC000) {
                        uint32_t offset = (uint32_t)(start_addr - 0xC000);
                        uint32_t n = (offset + data_size > MAX_DPCM_SAMPLE_BANK_SIZE)
                                   ? MAX_DPCM_SAMPLE_BANK_SIZE - offset : data_size;
                        buffer_reader_read(reader, w->dpcm + offset, n);
                        buffer_reader_skip(reader, data_size - n); // drop any overflow tail
                        if (offset + n > w->dpcm_size) {
                            w->dpcm_size = offset + n;
                        }
                    } else {
                        buffer_reader_skip(reader, data_size);
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
                    sample_accum += (cmd & 0x0F) + 1; // wait 1..16 samples
                } else {
                    // Unknown command: we can't know its operand length, so bail cleanly
                    // rather than desync. Extend the handled set as needed.
                    return FAM_ERROR_UNSUPPORTED_FEATURE;
                }
                break;
        }
        if (result != FAM_SUCCESS) {
            return result;
        }
        if (reader->error) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        // Flush any whole frames the accumulated waits have crossed
        if (sample_accum >= samples_per_frame) {
            uint32_t frames = sample_accum / samples_per_frame;
            sample_accum -= frames * samples_per_frame;
            result = emit_wait(w, frames);
            if (result != FAM_SUCCESS) {
                return result;
            }
        }
    }

    FamResult result = emit(w, OP_ENDSTREAM, 0);
    if (result != FAM_SUCCESS) {
        return result;
    }

    *out_loop_point = loop_point;
    return FAM_SUCCESS;
}

FamResult fam_music_from_vgm_buffer(FamMusic** out_music, size_t buffer_size, const uint8_t* buffer) {
    if (out_music == NULL || buffer == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    BufferReader header_reader = buffer_reader_init(buffer, buffer_size);
    VgmHeader hdr;
    FamResult result = parse_header(&header_reader, &hdr);
    if (result != FAM_SUCCESS) {
        return result;
    }

    uint8_t machine = machine_from_nes_clock(hdr.nes_clock);
    const uint32_t samples_per_frame = machine == FAM_MACHINE_NTSC ? VGM_SAMPLES_PER_FRAME_NTSC : VGM_SAMPLES_PER_FRAME_PAL;

    // Pass 1: count ops, find the loop point, and gather the DPCM sample RAM, so we can size
    // the allocation. The scratch bank is the full 16 KB $C000-$FFFF window.
    uint8_t* dpcm_scratch = (uint8_t*)calloc(MAX_DPCM_SAMPLE_BANK_SIZE, 1);
    if (dpcm_scratch == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    BufferReader count_reader = buffer_reader_init(buffer, buffer_size);
    StreamWriter counter = { .dpcm = dpcm_scratch };
    uint32_t loop_point;
    result = walk_commands(&count_reader, &hdr, &counter, &loop_point, samples_per_frame);
    if (result != FAM_SUCCESS) {
        free(dpcm_scratch);
        return result;
    }
    const uint32_t op_count = counter.count;
    const uint32_t bank_count = (counter.dpcm_size > 0) ? 1 : 0;

    printf("VGM: %u stream ops, loop point %u, channel mask %llu, %u DPCM bytes\n",
        op_count, loop_point, (unsigned long long)counter.channel_mask, counter.dpcm_size);

    // Single contiguous block: struct + bank array + bank data + stream (mirrors io.c).
    // NOTE: sizeof(FamMusic) must be a multiple of alignof(DPCMSampleBank); holds while it is a
    // multiple of 8. Bank data and the stream are byte-aligned, so they can follow freely.
    size_t memory_size = sizeof(FamMusic)
        + (size_t)bank_count * sizeof(DPCMSampleBank)
        + (size_t)counter.dpcm_size
        + (size_t)op_count * sizeof(StreamOperation);
    void* memory = malloc(memory_size);
    if (memory == NULL) {
        free(dpcm_scratch);
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    FamMusic* music = (FamMusic*)memory;
    music->channel_mask = counter.channel_mask;
    music->dpcm_sample_bank_count = bank_count;
    music->dpcm_sample_banks = NULL;
    music->stream_op_count = op_count;
    music->stream = NULL;
    music->loop_point = (loop_point != MUSIC_NO_LOOP && loop_point < op_count) ? loop_point : MUSIC_NO_LOOP;
    music->machine = machine;

    uint8_t* mem_pos = (uint8_t*)memory + sizeof(FamMusic);

    if (bank_count > 0) {
        music->dpcm_sample_banks = (DPCMSampleBank*)mem_pos;
        mem_pos += sizeof(DPCMSampleBank) * bank_count;

        music->dpcm_sample_banks[0].size = counter.dpcm_size;
        music->dpcm_sample_banks[0].data = mem_pos;
        memcpy(mem_pos, dpcm_scratch, counter.dpcm_size);
        mem_pos += counter.dpcm_size;
    }

    free(dpcm_scratch);

    if (op_count > 0) {
        music->stream = (StreamOperation*)mem_pos;
    }

    // Pass 2: emit the ops into the allocated stream (the bank is already built, so dpcm=NULL).
    BufferReader emit_reader = buffer_reader_init(buffer, buffer_size);
    StreamWriter writer = { .stream = music->stream };
    result = walk_commands(&emit_reader, &hdr, &writer, &loop_point, samples_per_frame);
    if (result != FAM_SUCCESS) {
        free(memory);
        return result;
    }

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
