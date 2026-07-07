#include <fam/ftm.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define MAGIC "FamiTracker Module"

#define VERSION_NEW 0x0200
#define VERSION_LATEST 0x0440

#define NOTE_COUNT 96
#define MAX_SEQUENCE_LENGTH 253

#define DEFAULT_TEMPO_NTSC 150
#define DEFAULT_TEMPO_PAL 125
#define DEFAULT_SPEED 6
#define DEFAULT_SPEED_SPLIT_POINT 32
#define OLD_SPEED_SPLIT_POINT 21

#define DEFAULT_NAMCO_CHANNELS 1

typedef enum {
    MACHINE_NTSC = 0,
    MACHINE_PAL
} Machine;

typedef enum {
    VIBRATO_OLD = 0,
    VIBRATO_NEW
} Vibrato;

typedef enum {
    INSTRUMENT_NONE = 0,
    INSTRUMENT_2A03 = 1,
    INSTRUMENT_VRC6,
    INSTRUMENT_VRC7,
    INSTRUMENT_FDS,
    INSTRUMENT_N163,
    INSTRUMENT_SSB
} InstrumentType;

typedef enum {
    EF_NONE = 0,
	EF_SPEED,
	EF_JUMP,
	EF_SKIP,
	EF_HALT,
	EF_VOLUME,
	EF_PORTAMENTO,
	EF_PORTAOFF,				// unused!!
	EF_SWEEPUP,
	EF_SWEEPDOWN,
	EF_ARPEGGIO,
	EF_VIBRATO,
	EF_TREMOLO,
	EF_PITCH,
	EF_DELAY,
	EF_DAC,
	EF_PORTA_UP,
	EF_PORTA_DOWN,
	EF_DUTY_CYCLE,
	EF_SAMPLE_OFFSET,
	EF_SLIDE_UP,
	EF_SLIDE_DOWN,
	EF_VOLUME_SLIDE,
	EF_NOTE_CUT,
	EF_RETRIGGER,
	EF_DELAYED_VOLUME,			// Unimplemented
	EF_FDS_MOD_DEPTH,
	EF_FDS_MOD_SPEED_HI,
	EF_FDS_MOD_SPEED_LO,
	EF_DPCM_PITCH,
	EF_SUNSOFT_ENV_LO,
	EF_SUNSOFT_ENV_HI,
	EF_SUNSOFT_ENV_TYPE,
//	EF_TARGET_VOLUME_SLIDE, 
/*
	EF_VRC7_MODULATOR,
	EF_VRC7_CARRIER,
	EF_VRC7_LEVELS,
*/
} Effect;

typedef enum {
    SOUNDCHIP_NONE               = 0,
    SOUNDCHIP_VRC6               = 1 << 0,
    SOUNDCHIP_VRC7               = 1 << 1,
    SOUNDCHIP_FDS                = 1 << 2,
    SOUNDCHIP_MMC5               = 1 << 3,
    SOUNDCHIP_N163               = 1 << 4,
    SOUNDCHIP_SSB                = 1 << 5
} SoundChipFlags;

typedef struct {
    uint8_t index   : 7;
    uint8_t enabled : 1;
} SequenceRef;

// Fat struct of all Famitracker instument data instead of inheritance
typedef struct {
    uint8_t type;
    char name[128];
    union {
        struct {
            SequenceRef volume, arpeggio, pitch, hi_pitch, duty_noise;
        };
        SequenceRef seq_raw[5];
    };
    
} Instrument;

typedef struct {
    uint8_t length;
    uint32_t loop_point;
    uint32_t release_point;
    uint32_t setting;
    int8_t values[MAX_SEQUENCE_LENGTH];
} SequenceEff;

typedef struct {
    union {
        struct {
            SequenceEff volume, arpeggio, pitch, hi_pitch, duty_noise;
        };
        SequenceEff eff_raw[5];
    };
} SequenceGroup;

typedef struct {
    uint8_t note;
    uint8_t octave;
    uint8_t volume;
    uint8_t instrument;
    uint8_t eff_number[4];
    uint8_t eff_param[4];
} Note;

typedef struct {
    uint32_t speed;
    uint32_t tempo;
    uint8_t* fx_counts; // Array of channel_count additional effect column counts
    uint32_t frame_count;
    uint8_t* frames; // Flat array of frame_count * channel_count pattern indices
    uint32_t pattern_length;
    uint32_t pattern_count;
    Note* patterns; // Flat array of pattern_count * channel_count * pattern_length notes
} Track;

typedef struct {
    uint8_t* const start;
    uint8_t* pos;
    uint8_t* const end;
    bool error;
} BlockReader;

typedef struct {
    uint32_t file_version;

    char name[32];
    char artist[32];
    char copyright[32];

    uint32_t global_speed;
    uint32_t global_tempo;
    uint8_t expansion_chip;
    uint32_t channel_count;
    uint32_t machine;
    uint32_t vibrato_style;
    uint32_t namco_channels;
    uint32_t speed_split_point;

    uint8_t track_count;
    Track* tracks;

    uint32_t instrument_count;
    Instrument* instruments;

    uint32_t sequence_count;
    SequenceGroup* sequences;
} Document;

static const uint32_t NOTE_TABLE_NTSC[NOTE_COUNT] = {
    0xD5B, 0xC9C, 0xBE6, 0xB3B, 0xA9A, 0xA01, 0x972, 0x8EA, 
    0x86A, 0x7F1, 0x77F, 0x713, 0x6AD, 0x64D, 0x5F3, 0x59D, 
    0x54C, 0x500, 0x4B8, 0x474, 0x434, 0x3F8, 0x3BF, 0x389, 
    0x356, 0x326, 0x2F9, 0x2CE, 0x2A6, 0x280, 0x25C, 0x23A, 
    0x21A, 0x1FB, 0x1DF, 0x1C4, 0x1AB, 0x193, 0x17C, 0x167, 
    0x152, 0x13F, 0x12D, 0x11C, 0x10C, 0x0FD, 0x0EF, 0x0E1, 
    0x0D5, 0x0C9, 0x0BD, 0x0B3, 0x0A9, 0x09F, 0x096, 0x08E, 
    0x086, 0x07E, 0x077, 0x070, 0x06A, 0x064, 0x05E, 0x059, 
    0x054, 0x04F, 0x04B, 0x046, 0x042, 0x03F, 0x03B, 0x038, 
    0x034, 0x031, 0x02F, 0x02C, 0x029, 0x027, 0x025, 0x023, 
    0x021, 0x01F, 0x01D, 0x01B, 0x01A, 0x018, 0x017, 0x015, 
    0x014, 0x013, 0x012, 0x011, 0x010, 0x00F, 0x00E, 0x00D, 
};

static const int32_t VIBRATO_TABLE_NEW[256] = {
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 
    0x000, 0x000, 0x000, 0x000, 0x000, 0x001, 0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 
    0x000, 0x000, 0x000, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x003, 0x003, 0x003, 0x003, 0x003, 0x003, 0x003, 
    0x000, 0x000, 0x000, 0x001, 0x001, 0x002, 0x002, 0x003, 0x003, 0x003, 0x004, 0x004, 0x004, 0x004, 0x004, 0x004, 
    0x000, 0x000, 0x001, 0x002, 0x002, 0x003, 0x003, 0x004, 0x004, 0x005, 0x005, 0x006, 0x006, 0x006, 0x006, 0x006, 
    0x000, 0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x007, 0x008, 0x008, 0x009, 0x009, 0x009, 0x009, 
    0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x009, 0x00A, 0x00B, 0x00B, 0x00B, 0x00B, 
    0x000, 0x001, 0x002, 0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x00A, 0x00B, 0x00C, 0x00C, 0x00D, 0x00D, 0x00D, 
    0x000, 0x001, 0x003, 0x004, 0x006, 0x008, 0x009, 0x00A, 0x00C, 0x00D, 0x00E, 0x00E, 0x00F, 0x010, 0x010, 0x010, 
    0x000, 0x002, 0x004, 0x006, 0x008, 0x00A, 0x00C, 0x00D, 0x00F, 0x011, 0x012, 0x013, 0x014, 0x015, 0x015, 0x015, 
    0x000, 0x002, 0x005, 0x008, 0x00B, 0x00E, 0x010, 0x013, 0x015, 0x017, 0x018, 0x01A, 0x01B, 0x01C, 0x01D, 0x01D, 
    0x000, 0x004, 0x008, 0x00C, 0x010, 0x014, 0x018, 0x01B, 0x01F, 0x022, 0x024, 0x026, 0x028, 0x02A, 0x02B, 0x02B, 
    0x000, 0x006, 0x00C, 0x012, 0x018, 0x01E, 0x023, 0x028, 0x02D, 0x031, 0x035, 0x038, 0x03B, 0x03D, 0x03E, 0x03F, 
    0x000, 0x009, 0x012, 0x01B, 0x024, 0x02D, 0x035, 0x03C, 0x043, 0x04A, 0x04F, 0x054, 0x058, 0x05B, 0x05E, 0x05F, 
    0x000, 0x00C, 0x018, 0x025, 0x030, 0x03C, 0x047, 0x051, 0x05A, 0x062, 0x06A, 0x070, 0x076, 0x07A, 0x07D, 0x07F, 
};

static inline void document_init(Document* doc) {
    *doc = (Document){0};
    doc->global_speed = DEFAULT_SPEED;
    doc->global_tempo = DEFAULT_TEMPO_NTSC;
    doc->namco_channels = DEFAULT_NAMCO_CHANNELS;
    doc->speed_split_point = OLD_SPEED_SPLIT_POINT;
}

static inline void document_free(Document* doc) {
    if (doc->tracks != NULL) {
        for (int i = 0; i < doc->track_count; i++) {
            free(doc->tracks[i].fx_counts);
            free(doc->tracks[i].frames);
            free(doc->tracks[i].patterns);
        }
    }
    free(doc->tracks);
    free(doc->instruments);
    free(doc->sequences);
    *doc = (Document){0};
}

static inline BlockReader block_reader_init(void* data, size_t size) {
    BlockReader reader = {
        .start = (uint8_t*)data,
        .pos = (uint8_t*)data,
        .end = (uint8_t*)data + size,
        .error = false
    };
    return reader;
}

static inline void block_read(BlockReader* reader, void* dst, size_t size) {
    if (reader->error) {
        return;
    }

    // Trying to read past block end
    if (reader->end - reader->pos < size) {
        reader->pos = reader->end;
        reader->error = true;
        return;
    }

    memcpy(dst, (void*)reader->pos, size);
    reader->pos += size;
}

static inline void block_skip(BlockReader* reader, size_t size) {
    if (reader->error) {
        return;
    }

    // Trying to skip past block end
    if (reader->end - reader->pos < size) {
        reader->pos = reader->end;
        reader->error = true;
        return;
    }

    reader->pos += size;
}

static FamResult block_read_params(BlockReader* reader, uint32_t block_version, Document* doc) {
    if (block_version == 1) {
        block_read(reader, &doc->global_speed, sizeof(uint32_t));
    } else {
        block_read(reader, &doc->expansion_chip, sizeof(uint8_t));
    }

    block_read(reader, &doc->channel_count, sizeof(uint32_t));

    block_read(reader, &doc->machine, sizeof(uint32_t));
    if (doc->machine != MACHINE_NTSC) {
        // TODO: PAL support
        return reader->error ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    uint32_t engine_speed;
    block_read(reader, &engine_speed, sizeof(uint32_t));
    if (engine_speed != 0) {
        // No fiddling with the engine speed allowed
        return reader->error ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    if (block_version > 2) {
        block_read(reader, &doc->vibrato_style, sizeof(uint32_t));
    } else {
        doc->vibrato_style = VIBRATO_OLD;
    }

    if (block_version > 3) {
        // Skip some UI highlight things
        block_skip(reader, sizeof(uint32_t) * 2);
    }

    // Famitracker bug (?) sometimes set expansion_chip value to 0xFF in files
    if (doc->channel_count == 5) {
        doc->expansion_chip = SOUNDCHIP_NONE;
    }

    // For simplicity, we don't allow multiple expansion chips at the same time
    if (doc->expansion_chip != SOUNDCHIP_NONE &&
        (doc->expansion_chip & (doc->expansion_chip - 1)) != 0) {
        return reader->error ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    if (doc->file_version == 0x0200 && doc->global_speed < 20) {
        doc->global_speed++;
    }

    if (block_version == 1) {
        if (doc->global_speed > 19) {
            doc->global_tempo = doc->global_speed;
            doc->global_speed = DEFAULT_SPEED;
        } else {
            doc->global_tempo = doc->machine == MACHINE_NTSC ? DEFAULT_TEMPO_NTSC : DEFAULT_TEMPO_PAL;
        }
    }

    if (block_version >= 5 && doc->expansion_chip & SOUNDCHIP_N163) {
        block_read(reader, &doc->namco_channels, sizeof(uint32_t));
        if (doc->namco_channels >= 9) {
            return FAM_ERROR_INVALID_FORMAT;
        }
    }

    if (block_version >= 6) {
        block_read(reader, &doc->speed_split_point, sizeof(uint32_t));
    } else {
        doc->speed_split_point = OLD_SPEED_SPLIT_POINT;
    }

    if (doc->expansion_chip != SOUNDCHIP_NONE && doc->machine != MACHINE_NTSC) {
        // Famitracker isn't this harsh, it just overrides machine
        return FAM_ERROR_INVALID_FORMAT;
    }

    // Overwriting channel_count, I guess the earlier read is just some old remnant...
    doc->channel_count = 5;
    if (doc->expansion_chip & SOUNDCHIP_VRC6) {
        doc->channel_count += 3;
    }
    if (doc->expansion_chip & SOUNDCHIP_VRC7) {
        doc->channel_count += 6;
    }
    if (doc->expansion_chip & SOUNDCHIP_FDS) {
        doc->channel_count++;
    }
    if (doc->expansion_chip & SOUNDCHIP_MMC5) {
        doc->channel_count += 2;
    }
    if (doc->expansion_chip & SOUNDCHIP_N163) {
        doc->channel_count += doc->namco_channels;
    }
    if (doc->expansion_chip & SOUNDCHIP_SSB) {
        doc->channel_count += 3;
    }

    printf("Channel count = %d\n", doc->channel_count);

    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_info(BlockReader* reader, uint32_t block_version, Document* doc) {
    block_read(reader, doc->name, sizeof(doc->name));
    printf("Document name: %s\n", doc->name);

    block_read(reader, doc->artist, sizeof(doc->artist));
    printf("Artist name: %s\n", doc->artist);

    block_read(reader, doc->copyright, sizeof(doc->copyright));
    printf("Copyright: %s\n", doc->copyright);
    
    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_header(BlockReader* reader, uint32_t block_version, Document* doc) {
    // We're using channel_count in this function to allocate memory,
    // so need to make sure it has been set first
    if (doc->channel_count == 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if (block_version == 1) {
        doc->track_count = 1;

        // Skip over unused channel type byte
        block_skip(reader, 1);

        doc->tracks = (Track*)calloc(1, sizeof(Track));
        if (doc->tracks == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }

        doc->tracks->fx_counts = (uint8_t*)calloc(doc->channel_count, sizeof(uint8_t));
        if (doc->tracks->fx_counts == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }
        block_read(reader, doc->tracks->fx_counts, sizeof(uint8_t) * doc->channel_count);
    } else {
        // Block version >= 2 supports multiple tracks
        block_read(reader, &doc->track_count, sizeof(uint8_t));
        doc->track_count++; // In file, 0 means one track
        printf("Track count: %d\n", doc->track_count);

        doc->tracks = (Track*)calloc(doc->track_count, sizeof(Track));
        if (doc->tracks == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }

        for (int i = 0; i < doc->track_count; i++) {
            doc->tracks[i].fx_counts = (uint8_t*)calloc(doc->channel_count, sizeof(uint8_t));
            if (doc->tracks[i].fx_counts == NULL) {
                return FAM_ERROR_OUT_OF_MEMORY;
            }

            if (block_version >= 3) {
                char c = 0;
                // Track name: Reading bytes until we get null
                // We probably don't need the name for anything...
                do {
                    block_read(reader, &c, 1);
                    printf("%c", c);
                } while (c != 0 && reader->pos < reader->end);
                printf("\n");
            }
        }

        // For some reason, track effect col counts are grouped by channel in the file
        // so we'll have to loop over tracks twice
        for (int i = 0; i < doc->channel_count; i++) {
            // Skip over unused channel type byte
            block_skip(reader, 1);

            for (int j = 0; j < doc->track_count; j++) {
                block_read(reader, &doc->tracks[j].fx_counts[i], sizeof(uint8_t));
            }
        }

        // The rest of HEADER is just UI highlight stuff, which we don't care about
    }

    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_instruments(BlockReader* reader, uint32_t block_version, Document* doc) {
    block_read(reader, &doc->instrument_count, sizeof(uint32_t));

    if (doc->instrument_count > 64) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    doc->instruments = (Instrument*)calloc(doc->instrument_count, sizeof(Instrument));
    if (doc->instruments == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }
    
    for (uint32_t i = 0; i < doc->instrument_count; i++) {
        int32_t index = -1;
        block_read(reader, &index, sizeof(int32_t));
        if (index < 0 || index >= doc->instrument_count) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        printf("Instrument #%d:\n", index);
        Instrument* instrument = doc->instruments + index;

        block_read(reader, &instrument->type, sizeof(uint8_t));

        switch (instrument->type) {
            case INSTRUMENT_2A03: {
                printf("Type: 2A03\n");

                int32_t seq_count = 0;
                block_read(reader, &seq_count, sizeof(int32_t));
                if (seq_count > 5) {
                    return FAM_ERROR_INVALID_FORMAT;
                }

                for (int32_t s = 0; s < seq_count; s++) {
                    uint8_t enabled;
                    block_read(reader, &enabled, sizeof(uint8_t));
                    uint8_t index;
                    block_read(reader, &index, sizeof(uint8_t));
                    if (index > 0x7F) {
                        return FAM_ERROR_INVALID_FORMAT;
                    }
                    instrument->seq_raw[s].enabled = enabled;
                    instrument->seq_raw[s].index = index;
                }

                int octave_range = block_version == 1 ? 6 : 8;
                block_skip(reader, octave_range*12* (block_version > 5 ? 3 : 2));
                break;
            }
            case INSTRUMENT_VRC6: {
                printf("Type: VRC6\n");

                int32_t seq_count = 0;
                block_read(reader, &seq_count, sizeof(int32_t));
                if (seq_count > 5) {
                    return FAM_ERROR_INVALID_FORMAT;
                }

                // For some reason, the seq_count is totally useless and we just read all 5 anyway
                for (int32_t s = 0; s < 5; s++) {
                    uint8_t enabled;
                    block_read(reader, &enabled, sizeof(uint8_t));
                    uint8_t index;
                    block_read(reader, &index, sizeof(uint8_t));
                    if (index > 0x7F) {
                        return FAM_ERROR_INVALID_FORMAT;
                    }
                    instrument->seq_raw[s].enabled = enabled;
                    instrument->seq_raw[s].index = index;
                }

                break;
            }
            case INSTRUMENT_VRC7:/* {
                break;
            }*/
            case INSTRUMENT_FDS:/* {
                break;
            }*/
            case INSTRUMENT_N163:/* {
                break;
            }*/
            case INSTRUMENT_NONE:
            case INSTRUMENT_SSB:
            default:
                // Invalid instrument type
                return FAM_ERROR_INVALID_FORMAT;
        }

        uint32_t name_len = 0;
        block_read(reader, &name_len, sizeof(uint32_t));
        if (name_len >= sizeof(instrument->name)) {
            return FAM_ERROR_INVALID_FORMAT;
        }
        block_read(reader, &instrument->name, sizeof(char) * name_len);
        instrument->name[name_len] = 0;
        printf("Name: %s\n", instrument->name);
    }

    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_sequences(BlockReader* reader, uint32_t block_version, Document* doc) {
    FamResult result = FAM_SUCCESS;
    uint32_t* seq_temp_indices = NULL;
    uint32_t* seq_temp_types = NULL;

    uint32_t total_count = 0;
    block_read(reader, &total_count, sizeof(uint32_t));

    // This doesn't actually ensure the count is valid, 
    // because it could still have too many sequences of a specific type.
    // That's why it's a bit silly to not have them grouped to begin with...
    if (total_count > 0x80 * 5) {
        result = FAM_ERROR_INVALID_FORMAT;
        goto cleanup;
    }

    // I can't determine the group count from total_count alone,
    // So I'll just dynamically realloc it....
    // I could also do multiple read iterations and only alloc once, but this'll do.
    // Famitracker doesn't have this problem, because its sequence array is always 128 entries
    doc->sequence_count = 0;

    printf("Sequences block version: %d\n", block_version);

    if (block_version == 1) {
        // TODO: Implement version 1
        result = FAM_ERROR_UNSUPPORTED_VERSION;
        goto cleanup;
    } else if (block_version == 2) {
        // TODO: Implement version 2
        result = FAM_ERROR_UNSUPPORTED_VERSION;
        goto cleanup;
    } else if (block_version >= 3) {
        // Because of weird decisions in block version 6, we need to temporarily
        // store the indices and types because that information is needed later...
        if (block_version >= 6) {
            seq_temp_indices = calloc(total_count, sizeof(uint32_t));
            seq_temp_types = calloc(total_count, sizeof(uint32_t));

            if (seq_temp_indices == NULL || seq_temp_types == NULL) {
                result = FAM_ERROR_OUT_OF_MEMORY;
                goto cleanup;
            }
        }

        for (uint32_t i = 0; i < total_count; i++) {
            uint32_t index, type;
            block_read(reader, &index, sizeof(uint32_t));
            block_read(reader, &type, sizeof(uint32_t));

            // This is the actual count validation
            if (index >= 0x80 || type >= 5) {
                result = FAM_ERROR_INVALID_FORMAT;
                goto cleanup;
            }

            if (index >= doc->sequence_count) {
                uint32_t old_count = doc->sequence_count;
                doc->sequence_count = index + 1;
                printf("Resizing sequences array from %d to %d\n", old_count, doc->sequence_count);
                void* new_sequences = realloc(doc->sequences, sizeof(SequenceGroup) * doc->sequence_count);
                if (new_sequences == NULL) {
                    result = FAM_ERROR_OUT_OF_MEMORY;
                    goto cleanup;
                }
                doc->sequences = new_sequences;
                memset((void*)(doc->sequences + old_count), 0, (doc->sequence_count - old_count) * sizeof(SequenceGroup));
            }

            SequenceEff* effect = &doc->sequences[index].eff_raw[type];

            block_read(reader, &effect->length, sizeof(uint8_t));
            block_read(reader, &effect->loop_point, sizeof(uint32_t));

            if (effect->length > MAX_SEQUENCE_LENGTH) {
                result = FAM_ERROR_INVALID_FORMAT;
                goto cleanup;
            }

            // Workaround for some older files
            if (effect->loop_point == effect->length) {
                // No loop sentinel, truncates to 0xFF when converted to uint8_t
                effect->loop_point = UINT32_MAX;
            }

            printf("Seq #%d: Index %d, Type %d, length %d, loop point %d\n", doc->sequence_count, index, type, effect->length, effect->loop_point);

            if (block_version == 4) {
                block_read(reader, &effect->release_point, sizeof(uint32_t));
                block_read(reader, &effect->setting, sizeof(uint32_t));
            } else if (block_version >= 6) {
                seq_temp_indices[i] = index;
                seq_temp_types[i] = type;
            }

            block_read(reader, effect->values, sizeof(int8_t) * effect->length);
        }

        if (block_version == 5) {
            // Version 5 had release points saved incorrectly, fixed in version 6
            // File contains 128*5 entries, but we can read sequence_count*5 and disregard rest
            for (int i = 0; i < doc->sequence_count; ++i) {
                for (int j = 0; j < 5; j++) {
                    SequenceEff* effect = &doc->sequences[i].eff_raw[j];
                    block_read(reader, &effect->release_point, sizeof(uint32_t));
                    block_read(reader, &effect->setting, sizeof(uint32_t));
                }
            }
        } else if (block_version >= 6) {
            // Honestly version 5 made more sense to me...
            // Now we need to temporarily store ieffect->ndices and types to do this
            for (uint32_t i = 0; i < total_count; i++) {
                uint32_t index = seq_temp_indices[i];
                uint32_t type = seq_temp_types[i];
                SequenceEff* effect = &doc->sequences[index].eff_raw[type];
                block_read(reader, &effect->release_point, sizeof(uint32_t));
                block_read(reader, &effect->setting, sizeof(uint32_t));
            }
        }
    }

cleanup:
    if (seq_temp_indices != NULL) {
        free(seq_temp_indices);
    }
    if (seq_temp_types != NULL) {
        free(seq_temp_types);
    }

    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return result;
}

static FamResult block_read_frames(BlockReader* reader, uint32_t block_version, Document* doc) {
    // We're expecting to have read HEADER first, where tracks are allocated
    if (doc->track_count == 0 || doc->tracks == NULL) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if (block_version == 1) {
        Track* track = &doc->tracks[0];

        block_read(reader, &track->frame_count, sizeof(uint32_t));

        track->speed = doc->global_speed;
        track->tempo = doc->global_tempo;

        // Suspicious: channel_count was already set when reading PARAMS, and now it's being read again here.
        // This suggests maybe there was no PARAMS block at all in file versions where the FRAMES block is ver 1...
        // Maybe the block order was different too. By now, we've already used the previous channel_count to allocate memory etc.
        // If there's an earlier conflicting value, I'm having it fail here:
        uint32_t ccount;
        block_read(reader, &ccount, sizeof(uint32_t));
        if (doc->channel_count != 0 && ccount != doc->channel_count) {
            return FAM_ERROR_INVALID_FORMAT;
        }
        doc->channel_count = ccount;

        track->frames = (uint8_t*)calloc(track->frame_count * doc->channel_count, sizeof(uint8_t));
        if (track->frames == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }

        // NOTE: Pattern length is read from the PATTERNS block,
        // and it's expected to also be ver 1

        for (uint32_t i = 0; i < track->frame_count; i++) {
            for (uint32_t j = 0; j < doc->channel_count; j++) {
                block_read(reader, &track->frames[doc->channel_count * i + j], sizeof(uint8_t));
            }
        }
    } else {
        for (int i = 0; i < doc->track_count; i++) {
            Track* track = &doc->tracks[i];

            block_read(reader, &track->frame_count, sizeof(uint32_t));
            block_read(reader, &track->speed, sizeof(uint32_t));

            track->frames = (uint8_t*)calloc(track->frame_count * doc->channel_count, sizeof(uint8_t));
            if (track->frames == NULL) {
                return FAM_ERROR_OUT_OF_MEMORY;
            }
            
            if (block_version == 3) {
                block_read(reader, &track->tempo, sizeof(uint32_t));
            } else if (track->speed < 20) {
                track->tempo = doc->machine == MACHINE_NTSC ? DEFAULT_TEMPO_NTSC : DEFAULT_TEMPO_PAL;
            } else {
                track->tempo = track->speed;
                track->speed = DEFAULT_SPEED;
            }

            printf("Track #%d: Frame count %d, speed %d, tempo %d\n", i, track->frame_count, track->speed, track->tempo);

            block_read(reader, &track->pattern_length, sizeof(uint32_t));

            for (uint32_t j = 0; j < track->frame_count; j++) {
                for (uint32_t k = 0; k < doc->channel_count; k++) {
                    block_read(reader, &track->frames[doc->channel_count * j + k], sizeof(uint8_t));
                }
            }
        }
    }
    
    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_patterns(BlockReader* reader, uint32_t block_version, Document* doc) {
    if (doc->channel_count == 0 || doc->track_count == 0 || doc->tracks == NULL) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    
    // Reading pattern length from this block instead of FRAMES in ver 1
    if (block_version == 1) {
        Track* track = &doc->tracks[0];
        uint32_t plength;
        block_read(reader, &plength, sizeof(uint32_t));
        // Just to be safe, make sure the value was not actually already read earlier:
        if (track->pattern_length != 0 && plength != track->pattern_length) {
            return FAM_ERROR_INVALID_FORMAT;
        }
        track->pattern_length = plength;
    }

    // We need to do the same realloc thing as before, because the file doesn't tell us how many patterns there are...
    while (reader->pos < reader->end) {
        uint32_t track_idx = 0;
        if (block_version > 1) {
            block_read(reader, &track_idx, sizeof(uint32_t));
        }

        uint32_t channel_idx, pattern_idx, pattern_length;
        block_read(reader, &channel_idx, sizeof(uint32_t));
        block_read(reader, &pattern_idx, sizeof(uint32_t));
        block_read(reader, &pattern_length, sizeof(uint32_t));

        printf("Track #%d: chan %d, pattern %d, pattern length %d\n", track_idx, channel_idx, pattern_idx, pattern_length);

        if (track_idx >= doc->track_count) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        Track* track = &doc->tracks[track_idx];

        if (channel_idx >= doc->channel_count ||
            pattern_idx >= 0x80 ||
            pattern_length > track->pattern_length) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        const size_t pattern_size = track->pattern_length * doc->channel_count;
        if (pattern_idx >= track->pattern_count) {
            uint32_t old_count = track->pattern_count;
            track->pattern_count = pattern_idx + 1;
            printf("Resizing patterns array from %d to %d\n", old_count, track->pattern_count);
            void* new_patterns = realloc(track->patterns, pattern_size * track->pattern_count * sizeof(Note));
            if (new_patterns == NULL) {
                return FAM_ERROR_OUT_OF_MEMORY;
            }
            track->patterns = new_patterns;
            memset((void*)(track->patterns + old_count * pattern_size), 0, (track->pattern_count - old_count) * pattern_size * sizeof(Note));
        }

        for (uint32_t i = 0; i < pattern_length; i++) {
            uint32_t row;
            if (doc->file_version == 0x0200) {
                uint8_t row_chr;
                block_read(reader, &row_chr, sizeof(uint8_t));
                row = row_chr;
            } else {
                block_read(reader, &row, sizeof(uint32_t));
            }

            if (row >= track->pattern_length) {
                return FAM_ERROR_INVALID_FORMAT;
            }
            
            Note* pattern = track->patterns + (track->pattern_length * doc->channel_count * pattern_idx);
            Note* channel = pattern + (track->pattern_length * channel_idx);
            Note* note = channel + row;

            block_read(reader, note, sizeof(uint8_t) * 4);

            // Only one effect column in file ver 2.0
            int fx_col_count = 1;
            if (doc->file_version != 0x0200) {
                // Note: fx_counts holds additional effect column count, so the total count is that +1
                fx_col_count = track->fx_counts[channel_idx] + 1;
            }

            for (int k = 0; k < fx_col_count; k++) {
                block_read(reader, &note->eff_number[k], sizeof(uint8_t));
                block_read(reader, &note->eff_param[k], sizeof(uint8_t));

                if (block_version < 3) {
                    if (note->eff_number[k] == EF_PORTAOFF) {
                        note->eff_number[k] = EF_PORTAMENTO;
                        note->eff_param[k] = 0;
                    } else if (note->eff_number[k] == EF_PORTAMENTO) {
                        if (note->eff_param[k] < 0xFF) {
                            note->eff_param[k]++;
                        }
                    }
                }
            }

            if (note->volume > 0x10) {
                note->volume &= 0x0F;
            }

            if (doc->file_version == 0x0200) {
                if (note->eff_number[0] == EF_SPEED && note->eff_param[0] < 20) {
                    note->eff_param[0]++;
                }

                if (note->volume == 0) {
                    note->volume = 0x10;
                } else {
                    note->volume--;
                    note->volume &= 0x0F;
                }

                if (note->note == 0) {
                    note->instrument = 64; // MAX_INSTRUMENTS
                }
            }

            if (block_version == 3) {
                // TODO: Mapper specific fixes
            }
            if (block_version < 5) {
                // TODO: FDS octave
            }
        }
    }

    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult parse_binary(const char* fname, Document* doc) {
    FILE *file = fopen(fname, "rb");
    if (file == NULL) {
        return FAM_ERROR_IO;
    }

    FamResult result = FAM_SUCCESS;
    
    document_init(doc);

    char block_id[16];
    uint32_t block_version;
    uint32_t block_size;
    void* block_data = NULL;

    // No null terminator included in file
    const size_t magic_size = sizeof(MAGIC) - 1;
    char magic[sizeof(MAGIC)];

    // Failure to read header
    if (fread(magic, sizeof(char), magic_size, file) < magic_size ||
        fread(&doc->file_version, sizeof(uint32_t), 1, file) == 0) {
        result = feof(file) ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_IO;
        goto cleanup;
    }

    // Magic doesn't match
    if (memcmp(magic, MAGIC, magic_size) != 0) {
        result = FAM_ERROR_INVALID_FORMAT;
        goto cleanup;
    }

    // Version is too old
    if (doc->file_version < VERSION_NEW) {
        result = FAM_ERROR_UNSUPPORTED_VERSION;
        goto cleanup;
    }

    // Version is too new
    if (doc->file_version > VERSION_LATEST) {
        result = FAM_ERROR_UNSUPPORTED_VERSION;
        goto cleanup;
    }

    // I've reproduced Famitracker's file reading logic pretty much as-is.
    // It assumes that the blocks are read in the correct order and that their
    // versions are compatible, without strictly enforcing those things.
    // For example, PARAMS needs to be read before HEADER, but theoretically
    // they could be in either order. HEADER version 1 implies FRAMES version 1 etc.
    // Without exhaustively researcing the format history, trying to make it
    // more strict would risk breaking compatibility though, so I opted not to do it.

    while (true) {
        size_t id_read = fread(block_id, 1, sizeof(block_id), file);
        if (id_read == 3 && memcmp(block_id, "END", 3) == 0) {
            break;
        }
        if (id_read < sizeof(block_id)) {
            result = feof(file) ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_IO;
            goto cleanup;
        }

        printf("%s\n", block_id);

        if (fread(&block_version, sizeof(uint32_t), 1, file) == 0 ||
            fread(&block_size, sizeof(uint32_t), 1, file) == 0) {
            result = feof(file) ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_IO;
            goto cleanup;
        }

        block_data = malloc(block_size);
        if (block_data == NULL) {
            result = FAM_ERROR_OUT_OF_MEMORY;
            goto cleanup;
        }

        if (fread(block_data, 1, block_size, file) != block_size) {
            result = feof(file) ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_IO;
            goto cleanup;
        }

        BlockReader reader = block_reader_init(block_data, block_size);

        if (strcmp(block_id, "PARAMS") == 0) {
            result = block_read_params(&reader, block_version, doc);
        } else if (strcmp(block_id, "INFO") == 0) {
            result = block_read_info(&reader, block_version, doc);
        } else if (strcmp(block_id, "HEADER") == 0) {
            result = block_read_header(&reader, block_version, doc);
        } else if (strcmp(block_id, "INSTRUMENTS") == 0) {
            result = block_read_instruments(&reader, block_version, doc);
        } else if (strcmp(block_id, "SEQUENCES") == 0) {
            result = block_read_sequences(&reader, block_version, doc);
        } else if (strcmp(block_id, "FRAMES") == 0) {
            result = block_read_frames(&reader, block_version, doc);
        } else if (strcmp(block_id, "PATTERNS") == 0) {
            result = block_read_patterns(&reader, block_version, doc);
        } else if (strcmp(block_id, "DPCM SAMPLES") == 0) {

        } else if (strcmp(block_id, "COMMENTS") == 0) {

        } else if (strcmp(block_id, "SEQUENCES_VRC6") == 0) {

        } else if (strcmp(block_id, "SEQUENCES_N163") == 0 ||
                   strcmp(block_id, "SEQUENCES_N106") == 0) {
            
        }

        free(block_data);
        block_data = NULL;

        if (result != FAM_SUCCESS) {
            goto cleanup;
        }
    }

cleanup:
    fclose(file);
    free(block_data);

    if (result != FAM_SUCCESS) {
        document_free(doc);
    }
    
    return result;
}

FamResult fam_load_music_ftm(const char* fname, size_t* out_count, FamMusic** out_tracks) {
    return FAM_ERROR_UNIMPLEMENTED;
}