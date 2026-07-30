#include <fam/ftm.h>
#include <fam/internal/ftm_types.h>
#include <fam/internal/grow_buffer.h>
#include <fam/internal/buffer_reader.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static FamResult block_read_params(BufferReader* reader, uint32_t block_version, FamFtModule* module) {
    if (block_version == 1) {
        buffer_reader_read(reader, &module->global_speed, sizeof(uint32_t));
    } else {
        buffer_reader_read(reader, &module->expansion_chip, sizeof(uint8_t));
    }

    uint32_t ccount;
    buffer_reader_read(reader, &ccount, sizeof(uint32_t));

    buffer_reader_read(reader, &module->machine, sizeof(uint32_t));
    if (module->machine != FT_MACHINE_NTSC) {
        // TODO: PAL support
        return reader->error ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    uint32_t engine_speed;
    buffer_reader_read(reader, &engine_speed, sizeof(uint32_t));
    if (engine_speed != 0) {
        // TODO: Support custom engine speed. It's rare, so rejected for now, but it
        // is hardware-reproducible: FamiTracker writes it to the NSF play-speed
        // header (1000000/speed us per tick) so the PLAY routine runs at that rate.
        // A custom speed doesn't change the row tempo (rows/sec = tempo / (2.5*speed)
        // is independent of the engine speed) - it raises the tick resolution, so the
        // baked stream has more frames per row and must be played back at that rate.
        // Supporting it means: an engine_speed field on FamMusic/FamSfx (ticks/sec,
        // 0 = default 60 Hz), carried through conversion, plus per-stream tick clocks
        // in the player so music and SFX can run at independent rates.
        return reader->error ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    if (block_version > 2) {
        buffer_reader_read(reader, &module->vibrato_style, sizeof(uint32_t));
    } else {
        module->vibrato_style = FT_VIBRATO_OLD;
    }

    if (block_version > 3) {
        // Skip some UI highlight things
        buffer_reader_skip(reader, sizeof(uint32_t) * 2);
    }

    // Famitracker bug (?) sometimes set expansion_chip value to 0xFF in files
    if (ccount == 5) {
        module->expansion_chip = FT_SOUNDCHIP_NONE;
    }

    // For simplicity, we don't allow multiple expansion chips at the same time
    if (module->expansion_chip != FT_SOUNDCHIP_NONE &&
        (module->expansion_chip & (module->expansion_chip - 1)) != 0) {
        return reader->error ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_UNSUPPORTED_FEATURE;
    }

    if (module->file_version == 0x0200 && module->global_speed < 20) {
        module->global_speed++;
    }

    if (block_version == 1) {
        if (module->global_speed > 19) {
            module->global_tempo = module->global_speed;
            module->global_speed = FT_DEFAULT_SPEED;
        } else {
            module->global_tempo = module->machine == FT_MACHINE_NTSC ? FT_DEFAULT_TEMPO_NTSC : FT_DEFAULT_TEMPO_PAL;
        }
    }

    if (block_version >= 5 && module->expansion_chip & FT_SOUNDCHIP_N163) {
        buffer_reader_read(reader, &module->namco_channels, sizeof(uint32_t));
        if (module->namco_channels >= 9) {
            return FAM_ERROR_INVALID_FORMAT;
        }
    }

    if (block_version >= 6) {
        buffer_reader_read(reader, &module->speed_split_point, sizeof(uint32_t));
    } else {
        module->speed_split_point = FT_OLD_SPEED_SPLIT_POINT;
    }

    if (module->expansion_chip != FT_SOUNDCHIP_NONE && module->machine != FT_MACHINE_NTSC) {
        // Famitracker isn't this harsh, it just overrides machine
        return FAM_ERROR_INVALID_FORMAT;
    }

    // Register channels
    module->channel_count = 0;
    module->channels[module->channel_count++] = FT_CHAN_ID_SQUARE1;
    module->channels[module->channel_count++] = FT_CHAN_ID_SQUARE2;
    module->channels[module->channel_count++] = FT_CHAN_ID_TRIANGLE;
    module->channels[module->channel_count++] = FT_CHAN_ID_NOISE;
    module->channels[module->channel_count++] = FT_CHAN_ID_DPCM;
    if (module->expansion_chip & FT_SOUNDCHIP_VRC6) {
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC6_PULSE1;
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC6_PULSE2;
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC6_SAWTOOTH;
    }
    if (module->expansion_chip & FT_SOUNDCHIP_MMC5) {
        module->channels[module->channel_count++] = FT_CHAN_ID_MMC5_SQUARE1;
        module->channels[module->channel_count++] = FT_CHAN_ID_MMC5_SQUARE2;
    }
    if (module->expansion_chip & FT_SOUNDCHIP_N163) {
        for (uint32_t i = 0; i < module->namco_channels; i++) {
            module->channels[module->channel_count++] = FT_CHAN_ID_N163_CHAN1 + i;
        }
    }
    if (module->expansion_chip & FT_SOUNDCHIP_FDS) {
        module->channels[module->channel_count++] = FT_CHAN_ID_FDS;
    }
    if (module->expansion_chip & FT_SOUNDCHIP_VRC7) {
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC7_CH1;
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC7_CH2;
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC7_CH3;
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC7_CH4;
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC7_CH5;
        module->channels[module->channel_count++] = FT_CHAN_ID_VRC7_CH6;
    }
    if (module->expansion_chip & FT_SOUNDCHIP_S5B) {
        // NOTE: Sunsoft 5B not supported by Famitracker
    }

    printf("Channel count = %d\n", module->channel_count);

    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_info(BufferReader* reader, uint32_t block_version, FamFtModule* module) {
    buffer_reader_read(reader, module->name, sizeof(module->name));
    printf("Document name: %s\n", module->name);

    buffer_reader_read(reader, module->artist, sizeof(module->artist));
    printf("Artist name: %s\n", module->artist);

    buffer_reader_read(reader, module->copyright, sizeof(module->copyright));
    printf("Copyright: %s\n", module->copyright);
    
    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_header(BufferReader* reader, uint32_t block_version, FamFtModule* module) {
    // We're using channel_count in this function to allocate memory,
    // so need to make sure it has been set first
    if (module->channel_count == 0) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if (block_version == 1) {
        module->track_count = 1;

        // Skip over unused channel type byte
        buffer_reader_skip(reader, 1);

        module->tracks = (FtTrack*)calloc(1, sizeof(FtTrack));
        if (module->tracks == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }
        module->tracks->channel_count = module->channel_count;

        module->tracks->fx_counts = (uint8_t*)calloc(module->channel_count, sizeof(uint8_t));
        if (module->tracks->fx_counts == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }
        buffer_reader_read(reader, module->tracks->fx_counts, sizeof(uint8_t) * module->channel_count);
    } else {
        // Block version >= 2 supports multiple tracks
        buffer_reader_read(reader, &module->track_count, sizeof(uint8_t));
        module->track_count++; // In file, 0 means one track
        printf("Track count: %d\n", module->track_count);

        if (module->track_count > FT_MAX_TRACKS) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        module->tracks = (FtTrack*)calloc(module->track_count, sizeof(FtTrack));
        if (module->tracks == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }

        for (int i = 0; i < module->track_count; i++) {
            module->tracks[i].channel_count = module->channel_count;
            module->tracks[i].fx_counts = (uint8_t*)calloc(module->channel_count, sizeof(uint8_t));
            if (module->tracks[i].fx_counts == NULL) {
                return FAM_ERROR_OUT_OF_MEMORY;
            }

            if (block_version >= 3) {
                char c = 0;
                // FtmTrack name: Reading bytes until we get null
                // We probably don't need the name for anything...
                do {
                    buffer_reader_read(reader, &c, 1);
                    printf("%c", c);
                } while (c != 0 && reader->pos < reader->end);
                printf("\n");
            }
        }

        // For some reason, track effect col counts are grouped by channel in the file
        // so we'll have to loop over tracks twice
        for (int i = 0; i < module->channel_count; i++) {
            // Skip over unused channel type byte
            buffer_reader_skip(reader, 1);

            for (int j = 0; j < module->track_count; j++) {
                buffer_reader_read(reader, &module->tracks[j].fx_counts[i], sizeof(uint8_t));
            }
        }

        // The rest of HEADER is just UI highlight stuff, which we don't care about
    }

    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_instruments(BufferReader* reader, uint32_t block_version, FamFtModule* module) {
    uint32_t instrument_count;
    buffer_reader_read(reader, &instrument_count, sizeof(uint32_t));

    if (instrument_count > FT_MAX_INSTRUMENTS) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    // Instrument indices are sparse within [0, FT_MAX_INSTRUMENTS); the fixed
    // module->instruments slot array is already zeroed. instrument_count is only how
    // many instruments are defined, not the highest index in use.
    for (uint32_t i = 0; i < instrument_count; i++) {
        int32_t index = -1;
        buffer_reader_read(reader, &index, sizeof(int32_t));
        if (index < 0 || index >= FT_MAX_INSTRUMENTS) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        printf("Instrument #%d:\n", index);
        FtInstrument* instrument = module->instruments + index;

        buffer_reader_read(reader, &instrument->type, sizeof(uint8_t));

        switch (instrument->type) {
            case FT_INSTRUMENT_2A03: {
                printf("Type: 2A03\n");

                int32_t seq_count = 0;
                buffer_reader_read(reader, &seq_count, sizeof(int32_t));
                if (seq_count > 5) {
                    return FAM_ERROR_INVALID_FORMAT;
                }

                for (int32_t s = 0; s < seq_count; s++) {
                    uint8_t enabled;
                    buffer_reader_read(reader, &enabled, sizeof(uint8_t));
                    uint8_t index;
                    buffer_reader_read(reader, &index, sizeof(uint8_t));
                    if (index > 0x7F) {
                        return FAM_ERROR_INVALID_FORMAT;
                    }
                    instrument->seq_raw[s].enabled = enabled;
                    instrument->seq_raw[s].index = index;
                }

                // Per-note DPCM sample assignments. Older files store only 6
                // octaves; the rest stay zeroed (= no sample assigned).
                int octave_range = block_version == 1 ? 6 : FT_OCTAVE_RANGE;
                for (int oct = 0; oct < octave_range; oct++) {
                    for (int n = 0; n < 12; n++) {
                        FtDPCMNote* dpcm_note = &instrument->dpcm_notes[oct][n];

                        uint8_t sample_index;
                        buffer_reader_read(reader, &sample_index, sizeof(uint8_t));
                        // FamiTracker treats out-of-range indices as "none"
                        if (sample_index > FT_MAX_DPCM_SAMPLES) {
                            sample_index = -1;
                        }
                        dpcm_note->sample_index = sample_index;

                        uint8_t pitch_byte;
                        buffer_reader_read(reader, &pitch_byte, sizeof(uint8_t));
                        dpcm_note->pitch = pitch_byte;
                        dpcm_note->loop = pitch_byte >> 7;

                        if (block_version > 5) {
                            buffer_reader_read(reader, &dpcm_note->delta, sizeof(int8_t));
                        } else {
                            dpcm_note->delta = -1;
                        }
                    }
                }
                break;
            }
            case FT_INSTRUMENT_VRC6: {
                printf("Type: VRC6\n");

                int32_t seq_count = 0;
                buffer_reader_read(reader, &seq_count, sizeof(int32_t));
                if (seq_count > 5) {
                    return FAM_ERROR_INVALID_FORMAT;
                }

                // For some reason, the seq_count is totally useless and we just read all 5 anyway
                for (int32_t s = 0; s < 5; s++) {
                    uint8_t enabled;
                    buffer_reader_read(reader, &enabled, sizeof(uint8_t));
                    uint8_t index;
                    buffer_reader_read(reader, &index, sizeof(uint8_t));
                    if (index > 0x7F) {
                        return FAM_ERROR_INVALID_FORMAT;
                    }
                    instrument->seq_raw[s].enabled = enabled;
                    instrument->seq_raw[s].index = index;
                }

                break;
            }
            case FT_INSTRUMENT_VRC7:/* {
                break;
            }*/
            case FT_INSTRUMENT_FDS:/* {
                break;
            }*/
            case FT_INSTRUMENT_N163:/* {
                break;
            }*/
            case FT_INSTRUMENT_NONE:
            case FT_INSTRUMENT_SSB:
            default:
                // Invalid instrument type
                return FAM_ERROR_INVALID_FORMAT;
        }

        uint32_t name_len = 0;
        buffer_reader_read(reader, &name_len, sizeof(uint32_t));
        if (name_len >= sizeof(instrument->name)) {
            return FAM_ERROR_INVALID_FORMAT;
        }
        buffer_reader_read(reader, &instrument->name, sizeof(char) * name_len);
        instrument->name[name_len] = 0;
        printf("Name: %s\n", instrument->name);
    }

    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

static FamResult block_read_sequences(BufferReader* reader, uint32_t block_version, FamFtModule* module) {
    FamResult result = FAM_SUCCESS;
    uint32_t* seq_temp_indices = NULL;
    uint32_t* seq_temp_types = NULL;

    uint32_t total_count = 0;
    buffer_reader_read(reader, &total_count, sizeof(uint32_t));

    // This doesn't actually ensure the count is valid, 
    // because it could still have too many sequences of a specific type.
    // That's why it's a bit silly to not have them grouped to begin with...
    if (total_count > 0x80 * 5) {
        result = FAM_ERROR_INVALID_FORMAT;
        goto cleanup;
    }

    // Like FamiTracker, use a fixed 128-entry sequence array (embedded in the
    // document, already zeroed); indices are sparse within it.
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
            buffer_reader_read(reader, &index, sizeof(uint32_t));
            buffer_reader_read(reader, &type, sizeof(uint32_t));

            // This is the actual count validation
            if (index >= 0x80 || type >= 5) {
                result = FAM_ERROR_INVALID_FORMAT;
                goto cleanup;
            }

            FtSequenceEff* effect = &module->sequences[index].eff_raw[type];

            buffer_reader_read(reader, &effect->length, sizeof(uint8_t));
            buffer_reader_read(reader, &effect->loop_point, sizeof(uint32_t));

            if (effect->length > FT_MAX_SEQUENCE_LENGTH) {
                result = FAM_ERROR_INVALID_FORMAT;
                goto cleanup;
            }

            // Workaround for some older files
            if (effect->loop_point == effect->length) {
                // No loop sentinel, truncates to 0xFF when converted to uint8_t
                effect->loop_point = UINT32_MAX;
            }

            printf("Seq: Index %d, Type %d, length %d, loop point %d\n", index, type, effect->length, effect->loop_point);

            if (block_version == 4) {
                buffer_reader_read(reader, &effect->release_point, sizeof(uint32_t));
                buffer_reader_read(reader, &effect->setting, sizeof(uint32_t));
            } else if (block_version >= 6) {
                seq_temp_indices[i] = index;
                seq_temp_types[i] = type;
            }

            buffer_reader_read(reader, effect->values, sizeof(int8_t) * effect->length);
        }

        if (block_version == 5) {
            // Version 5 had release points saved incorrectly, fixed in version 6.
            // The file stores all FT_MAX_SEQUENCES * 5 entries, so read them all
            // (like FamiTracker); entries for unused sequences land in slots that
            // are never referenced.
            for (int i = 0; i < FT_MAX_SEQUENCES; ++i) {
                for (int j = 0; j < 5; j++) {
                    FtSequenceEff* effect = &module->sequences[i].eff_raw[j];
                    buffer_reader_read(reader, &effect->release_point, sizeof(uint32_t));
                    buffer_reader_read(reader, &effect->setting, sizeof(uint32_t));
                }
            }
        } else if (block_version >= 6) {
            // Honestly version 5 made more sense to me...
            // Now we need to temporarily store ieffect->ndices and types to do this
            for (uint32_t i = 0; i < total_count; i++) {
                uint32_t index = seq_temp_indices[i];
                uint32_t type = seq_temp_types[i];
                FtSequenceEff* effect = &module->sequences[index].eff_raw[type];
                buffer_reader_read(reader, &effect->release_point, sizeof(uint32_t));
                buffer_reader_read(reader, &effect->setting, sizeof(uint32_t));
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

static FamResult block_read_frames(BufferReader* reader, uint32_t block_version, FamFtModule* module) {
    // We're expecting to have read HEADER first, where tracks are allocated
    if (module->track_count == 0 || module->tracks == NULL) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    if (block_version == 1) {
        FtTrack* track = &module->tracks[0];

        buffer_reader_read(reader, &track->frame_count, sizeof(uint32_t));

        track->speed = module->global_speed;
        track->tempo = module->global_tempo;

        // NOTE: High potential for bugs here as frames in v1 have their own independent channel count...
        uint32_t ccount;
        buffer_reader_read(reader, &ccount, sizeof(uint32_t));
        if (ccount > module->channel_count) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        // Memory allocation is still done based on the actual document channel count, 
        // but reading from the block based on the block's channel count
        track->frames = (uint8_t*)calloc(track->frame_count * module->channel_count, sizeof(uint8_t));
        if (track->frames == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }

        // NOTE: Pattern length is read from the PATTERNS block,
        // and it's expected to also be ver 1

        for (uint32_t i = 0; i < track->frame_count; i++) {
            for (uint32_t j = 0; j < ccount; j++) {
                buffer_reader_read(reader, &track->frames[module->channel_count * i + j], sizeof(uint8_t));
            }
        }
    } else {
        for (int i = 0; i < module->track_count; i++) {
            FtTrack* track = &module->tracks[i];

            buffer_reader_read(reader, &track->frame_count, sizeof(uint32_t));
            buffer_reader_read(reader, &track->speed, sizeof(uint32_t));

            track->frames = (uint8_t*)calloc(track->frame_count * module->channel_count, sizeof(uint8_t));
            if (track->frames == NULL) {
                return FAM_ERROR_OUT_OF_MEMORY;
            }
            
            if (block_version == 3) {
                buffer_reader_read(reader, &track->tempo, sizeof(uint32_t));
            } else if (track->speed < 20) {
                track->tempo = module->machine == FT_MACHINE_NTSC ? FT_DEFAULT_TEMPO_NTSC : FT_DEFAULT_TEMPO_PAL;
            } else {
                track->tempo = track->speed;
                track->speed = FT_DEFAULT_SPEED;
            }

            printf("Track #%d: Frame count %d, speed %d, tempo %d\n", i, track->frame_count, track->speed, track->tempo);

            buffer_reader_read(reader, &track->pattern_length, sizeof(uint32_t));

            for (uint32_t j = 0; j < track->frame_count; j++) {
                for (uint32_t k = 0; k < module->channel_count; k++) {
                    buffer_reader_read(reader, &track->frames[module->channel_count * j + k], sizeof(uint8_t));
                }
            }
        }
    }
    
    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }
    return FAM_SUCCESS;
}

// On-disk byte size of a single pattern item (one row of data).
static inline uint32_t pattern_item_size(const FamFtModule* module, const FtTrack* track, uint32_t channel_idx) {
    uint32_t row_size = (module->file_version == 0x0200) ? 1 : 4;
    uint32_t fx_cols = (module->file_version == 0x0200) ? 1 : (uint32_t)track->fx_counts[channel_idx] + 1;
    return row_size + 4 + 2 * fx_cols; // row + (note,octave,instrument,volume) + effect columns
}

static FamResult block_read_patterns(BufferReader* reader, uint32_t block_version, FamFtModule* module) {
    if (module->channel_count == 0 || module->track_count == 0 || module->tracks == NULL) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    // Reading pattern length from this block instead of FRAMES in ver 1
    if (block_version == 1) {
        FtTrack* track = &module->tracks[0];
        uint32_t plength;
        buffer_reader_read(reader, &plength, sizeof(uint32_t));
        // Just to be safe, make sure the value was not actually already read earlier:
        if (track->pattern_length != 0 && plength != track->pattern_length) {
            return FAM_ERROR_INVALID_FORMAT;
        }
        track->pattern_length = plength;
    }

    // The block lists (track, channel, pattern) entries with their non-empty rows
    // but never states how many distinct patterns exist. So we scan the entries
    // once to size each track's dense pattern pool, then rescan to fill it.
    const size_t entries_start = (size_t)(reader->pos - reader->start);

    // --- Pass 1: count the patterns each track uses, and validate headers ---
    uint32_t pool_count[FT_MAX_TRACKS] = {0};
    while (reader->pos < reader->end && !reader->error) {
        uint32_t track_idx = 0;
        if (block_version > 1) {
            buffer_reader_read(reader, &track_idx, sizeof(uint32_t));
        }

        uint32_t channel_idx, pattern_idx, item_count;
        buffer_reader_read(reader, &channel_idx, sizeof(uint32_t));
        buffer_reader_read(reader, &pattern_idx, sizeof(uint32_t));
        buffer_reader_read(reader, &item_count, sizeof(uint32_t));
        if (reader->error) {
            break;
        }

        if (track_idx >= module->track_count) {
            return FAM_ERROR_INVALID_FORMAT;
        }
        FtTrack* track = &module->tracks[track_idx];

        if (channel_idx >= track->channel_count ||
            pattern_idx >= FT_MAX_PATTERN ||
            track->pattern_length > FT_MAX_PATTERN_LENGTH ||
            item_count > track->pattern_length) {
            return FAM_ERROR_INVALID_FORMAT;
        }

        pool_count[track_idx]++;
        buffer_reader_skip(reader, (size_t)item_count * pattern_item_size(module, track, channel_idx));
    }
    if (reader->error) {
        return FAM_ERROR_INVALID_FORMAT;
    }

    // --- Allocate each track's sparse pattern map and dense pool ---
    for (uint32_t t = 0; t < module->track_count; t++) {
        FtTrack* track = &module->tracks[t];
        track->pattern_map = (FtNote**)calloc((size_t)track->channel_count * FT_MAX_PATTERN, sizeof(FtNote*));
        if (track->pattern_map == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }
        if (pool_count[t] > 0) {
            track->pattern_pool = (FtNote*)calloc((size_t)pool_count[t] * track->pattern_length, sizeof(FtNote));
            if (track->pattern_pool == NULL) {
                return FAM_ERROR_OUT_OF_MEMORY;
            }
            // Rows not present in the file stay at these defaults, which mean "no change":
            // an empty volume column is 0x10 (> 0x0F) and an empty instrument column is
            // FT_MAX_INSTRUMENTS. Leaving them calloc-zeroed would instead read as an
            // explicit "volume 0 / instrument 0" on every empty row.
            const size_t note_count = (size_t)pool_count[t] * track->pattern_length;
            for (size_t n = 0; n < note_count; n++) {
                track->pattern_pool[n].volume = 0x10;
                track->pattern_pool[n].instrument = FT_MAX_INSTRUMENTS;
            }
        }
    }

    // --- Pass 2: rescan and read the actual pattern data ---
    buffer_reader_seek(reader, entries_start);
    uint32_t pool_used[FT_MAX_TRACKS] = {0};
    while (reader->pos < reader->end && !reader->error) {
        uint32_t track_idx = 0;
        if (block_version > 1) {
            buffer_reader_read(reader, &track_idx, sizeof(uint32_t));
        }

        uint32_t channel_idx, pattern_idx, item_count;
        buffer_reader_read(reader, &channel_idx, sizeof(uint32_t));
        buffer_reader_read(reader, &pattern_idx, sizeof(uint32_t));
        buffer_reader_read(reader, &item_count, sizeof(uint32_t));
        if (reader->error) {
            break;
        }

        // Bounds were validated in pass 1
        FtTrack* track = &module->tracks[track_idx];

        printf("Track #%d: chan %d, pattern %d, items %d\n", track_idx, channel_idx, pattern_idx, item_count);

        // Claim a dense pool slot the first time we encounter this (channel, pattern)
        FtNote** slot = &track->pattern_map[channel_idx * FT_MAX_PATTERN + pattern_idx];
        if (*slot == NULL) {
            *slot = track->pattern_pool + (size_t)pool_used[track_idx] * track->pattern_length;
            pool_used[track_idx]++;
        }
        FtNote* pattern = *slot;

        for (uint32_t i = 0; i < item_count; i++) {
            uint32_t row;
            if (module->file_version == 0x0200) {
                uint8_t row_chr;
                buffer_reader_read(reader, &row_chr, sizeof(uint8_t));
                row = row_chr;
            } else {
                buffer_reader_read(reader, &row, sizeof(uint32_t));
            }

            if (row >= track->pattern_length) {
                return FAM_ERROR_INVALID_FORMAT;
            }

            FtNote* note = pattern + row;

            buffer_reader_read(reader, note, sizeof(uint8_t) * 4);

            // Only one effect column in file ver 2.0
            int fx_col_count = 1;
            if (module->file_version != 0x0200) {
                // FtmNote: fx_counts holds additional effect column count, so the total count is that +1
                fx_col_count = track->fx_counts[channel_idx] + 1;
            }

            for (int k = 0; k < fx_col_count; k++) {
                buffer_reader_read(reader, &note->eff_number[k], sizeof(uint8_t));
                buffer_reader_read(reader, &note->eff_param[k], sizeof(uint8_t));

                if (block_version < 3) {
                    if (note->eff_number[k] == FT_EF_PORTAOFF) {
                        note->eff_number[k] = FT_EF_PORTAMENTO;
                        note->eff_param[k] = 0;
                    } else if (note->eff_number[k] == FT_EF_PORTAMENTO) {
                        if (note->eff_param[k] < 0xFF) {
                            note->eff_param[k]++;
                        }
                    }
                }
            }

            if (note->volume > 0x10) {
                note->volume &= 0x0F;
            }

            if (module->file_version == 0x0200) {
                if (note->eff_number[0] == FT_EF_SPEED && note->eff_param[0] < 20) {
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

static FamResult block_read_dpcm_samples(BufferReader* reader, uint32_t block_version, FamFtModule* module) {
    uint8_t sample_count;
    buffer_reader_read(reader, &sample_count, sizeof(uint8_t));
    if (sample_count > FT_MAX_DPCM_SAMPLES) {
        printf("Sample count %d exceeds maximum sample count (%d)\n", sample_count, FT_MAX_DPCM_SAMPLES);
        return FAM_ERROR_INVALID_FORMAT;
    }

    printf("DPCM sample count: %d\n", sample_count);

    // Sample indices are sparse within [0, FT_MAX_DPCM_SAMPLES); the fixed
    // module->samples slot array is already zeroed.
    GrowBuffer sample_buffer;
    FamResult err = grow_buffer_init(&sample_buffer, 0x4000);
    if (err != FAM_SUCCESS) {
        return err;
    }

    // The buffer holds each sample's raw bytes back-to-back with no padding; the
    // 64-byte alignment the NES requires is applied later, per bank, during
    // conversion. Each sample records its byte offset into the buffer.
    uint32_t offset = 0;
    for (int i = 0; i < sample_count; i++) {
        uint8_t index;
        buffer_reader_read(reader, &index, sizeof(uint8_t));
        if (index >= FT_MAX_DPCM_SAMPLES) {
            printf("Sample index %d exceeds maximum sample count (%d)\n", index, FT_MAX_DPCM_SAMPLES);
            return FAM_ERROR_INVALID_FORMAT;
        }

        FtDPCMSample* sample = module->samples + index;

        uint32_t name_length;
        buffer_reader_read(reader, &name_length, sizeof(uint32_t));
        if (name_length >= 256) {
            printf("Sample #%d name length %d exceeds maximum name length (256)\n", index, name_length);
            return FAM_ERROR_INVALID_FORMAT;
        }

        buffer_reader_read(reader, sample->name, name_length);
        sample->name[name_length] = 0;
        printf("Sample #%d name: %s\n", index, sample->name);

        buffer_reader_read(reader, &sample->length, sizeof(uint32_t));
        if (sample->length > 0xFF1) {
            printf("Sample #%d length %d exceeds maximum length (0xFF1)\n", index, sample->length);
            return FAM_ERROR_INVALID_FORMAT;
        }

        printf("Sample #%d length: %d\n", index, sample->length);

        // Copy the raw sample bytes straight into the buffer (bounds-checked first,
        // since we read from the cursor position directly).
        if (sample->length > buffer_reader_remaining(reader)) {
            return FAM_ERROR_INVALID_FORMAT;
        }
        err = grow_buffer_write_bytes(&sample_buffer, reader->pos, sample->length);
        if (err != FAM_SUCCESS) {
            return err;
        }
        buffer_reader_skip(reader, sample->length);

        sample->offset = offset;
        offset += sample->length;
    }

    printf("Sample buffer size: %zu\n", sample_buffer.size);
    module->sample_data = sample_buffer.data;

    return FAM_SUCCESS;
}

static inline void track_free(FtTrack* track) {
    free(track->fx_counts);
    free(track->frames);
    free(track->pattern_map);
    free(track->pattern_pool);
}

static inline void module_init(FamFtModule* module) {
    *module = (FamFtModule){0};
    module->global_speed = FT_DEFAULT_SPEED;
    module->global_tempo = FT_DEFAULT_TEMPO_NTSC;
    module->namco_channels = FT_DEFAULT_NAMCO_CHANNELS;
    module->speed_split_point = FT_OLD_SPEED_SPLIT_POINT;
}

static FamResult module_parse_binary(const char* fname, FamFtModule* module) {
    FILE *file = fopen(fname, "rb");
    if (file == NULL) {
        return FAM_ERROR_IO;
    }

    FamResult result = FAM_SUCCESS;
    
    module_init(module);

    char block_id[16];
    uint32_t block_version;
    uint32_t block_size;
    void* block_data = NULL;

    // No null terminator included in file
    const size_t magic_size = sizeof(FT_MAGIC) - 1;
    char magic[sizeof(FT_MAGIC)];

    // Failure to read header
    if (fread(magic, sizeof(char), magic_size, file) < magic_size ||
        fread(&module->file_version, sizeof(uint32_t), 1, file) == 0) {
        result = feof(file) ? FAM_ERROR_INVALID_FORMAT : FAM_ERROR_IO;
        goto cleanup;
    }

    // Magic doesn't match
    if (memcmp(magic, FT_MAGIC, magic_size) != 0) {
        result = FAM_ERROR_INVALID_FORMAT;
        goto cleanup;
    }

    // Version is too old
    if (module->file_version < FT_VERSION_NEW) {
        result = FAM_ERROR_UNSUPPORTED_VERSION;
        goto cleanup;
    }

    // Version is too new
    if (module->file_version > FT_VERSION_LATEST) {
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

        BufferReader reader = buffer_reader_init(block_data, block_size);

        if (strcmp(block_id, "PARAMS") == 0) {
            result = block_read_params(&reader, block_version, module);
        } else if (strcmp(block_id, "INFO") == 0) {
            result = block_read_info(&reader, block_version, module);
        } else if (strcmp(block_id, "HEADER") == 0) {
            result = block_read_header(&reader, block_version, module);
        } else if (strcmp(block_id, "INSTRUMENTS") == 0) {
            result = block_read_instruments(&reader, block_version, module);
        } else if (strcmp(block_id, "SEQUENCES") == 0) {
            result = block_read_sequences(&reader, block_version, module);
        } else if (strcmp(block_id, "FRAMES") == 0) {
            result = block_read_frames(&reader, block_version, module);
        } else if (strcmp(block_id, "PATTERNS") == 0) {
            result = block_read_patterns(&reader, block_version, module);
        } else if (strcmp(block_id, "DPCM SAMPLES") == 0) {
            result = block_read_dpcm_samples(&reader, block_version, module);
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
    
    return result;
}

FamResult fam_ftmodule_from_file(FamFtModule** out_module, const char* fname) {
    if (out_module == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    *out_module = NULL;

    FamFtModule* module = (FamFtModule*)calloc(1, sizeof(FamFtModule));
    if (module == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    FamResult result = module_parse_binary(fname, module);
    if (result != FAM_SUCCESS) {
        fam_ftmodule_free(module);
        return result;
    }

    *out_module = module;
    return FAM_SUCCESS;
}

size_t fam_ftmodule_track_count(const FamFtModule* module) {
    if (module == NULL) return 0;

    return module->track_count;
}

void fam_ftmodule_free(FamFtModule* module) {
    free(module->sample_data);

    if (module->tracks != NULL) {
        for (int i = 0; i < module->track_count; i++) {
            track_free(&module->tracks[i]);
        }
    }
    free(module->tracks);
    free(module);
}