#include <fam/apu.h>
#include <stdlib.h>

#define NES_CPU_FREQ_NTSC 1789773

#define QUARTER_FRAME_CLOCK 3729
#define HALF_FRAME_CLOCK 7457
#define THREEQUARTERS_FRAME_CLOCK 11186
#define FRAME_CLOCK 14915
#define FRAME_CLOCK_MODE1 18641

static const uint8_t PULSE_SEQ[4] = {
    0b00000001,
    0b00000011,
    0b00001111,
    0b11111100
};

static const uint8_t TRIANGLE_SEQ[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const uint8_t LENGTH_TABLE[32] = {
    10, 254, 20, 2, 40, 4, 80, 6,
    160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22,
    192, 24, 72, 26, 16, 28, 32, 30
};

// NOTE: These noise period values are half what is written on Nesdev, 
// because they're APU cycles instead of CPU cycles
static const uint16_t NOISE_PERIOD_NTSC[16] = {
    2, 4, 8, 16, 32, 48, 64, 80, 101, 127, 190, 254, 381, 508, 1017, 2034
};

static const uint16_t NOISE_PERIOD_PAL[16] = {
    2, 4, 7, 15, 30, 44, 59, 74, 94, 118, 177, 236, 354, 472, 945, 1889
};

static const uint16_t DMC_RATE_NTSC[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54
};

static const uint16_t DMC_RATE_PAL[16] = {
    398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50
};

typedef struct PulseChannel {
    union {
        struct {
            uint8_t volume_envelope_period : 4;
            uint8_t constant_volume : 1;
            uint8_t loop : 1;
            uint8_t duty_cycle : 2;

            uint8_t sweep_shift : 3;
            uint8_t sweep_negate : 1;
            uint8_t sweep_period : 3;
            uint8_t sweep_enabled : 1;

            uint16_t timer_period : 11;
            uint16_t length_counter_load : 5;
        };
        uint8_t raw_registers[4];
    };

    uint16_t timer_counter;

    uint8_t sequence_pos : 3;
    uint8_t length_counter;

    uint8_t sweep_counter;
    uint16_t sweep_target_period;

    int8_t envelope_counter;
    uint8_t envelope_volume;
} PulseChannel;

typedef struct TriangleChannel {
    union {
        struct {
            uint8_t linear_counter_load : 7;
            uint8_t loop : 1;

            uint8_t unused;

            uint16_t timer_period : 11;
            uint16_t length_counter_load : 5;
        };
        uint8_t raw_registers[4];
    };

    uint16_t timer_counter;

    uint8_t sequence : 5;
    uint8_t length_counter;

    uint8_t linear_counter;

    bool halt;

} TriangleChannel;

typedef struct NoiseChannel {
    union {
        struct {
            uint8_t volume_envelope_period : 4;
            uint8_t constant_volume : 1;
            uint8_t loop : 1;
            uint8_t unused0 : 2;
            
            uint8_t unused1;

            uint8_t period : 4;
            uint8_t unused2 : 3;
            uint8_t mode : 1;

            uint8_t unused3 : 3;
            uint8_t length_counter_load : 5;
        };
        uint8_t raw_registers[4];
    };

    uint16_t timer_counter;

    uint16_t shift_register : 15;

    uint8_t length_counter;

    int8_t envelope_counter;
    uint8_t envelope_volume;
} NoiseChannel;

typedef struct DPCMChannel {
    union {
        struct {
            uint8_t sample_rate : 4;
            uint8_t unused0 : 2;
            uint8_t loop : 1;
            uint8_t enable_irq : 1;

            uint8_t direct_load : 7;
            uint8_t unused1 : 1; // Unused sign bit for direct load

            uint8_t sample_address;

            uint8_t sample_length;
        };
        uint8_t raw_registers[4];
    };

    uint16_t timer_counter;

    uint16_t current_address;
    uint16_t bytes_remaining;
    uint8_t sample_buffer;
    bool buffer_filled;

    uint8_t shift_register;
    uint8_t bits_remaining;

    uint8_t output_level : 7;
    uint8_t playing : 1;

    FamDmcReadFn reader;
    void* reader_data;
} DPCMChannel;

typedef struct StatusRegister {
    uint8_t enable_pulse1 : 1;
    uint8_t enable_pulse2 : 1;
    uint8_t enable_triangle : 1;
    uint8_t enable_noise : 1;
    uint8_t enable_dmc : 1;
    uint8_t unused : 1; // NOTE: Open bus bit, not implemented
    uint8_t frame_interrupt : 1;
    uint8_t dmc_interrupt : 1;
} StatusRegister;

struct FamApu {
    PulseChannel pulse[2];
    TriangleChannel triangle;
    NoiseChannel noise;
    DPCMChannel dmc;
    union {
        StatusRegister status;
        uint8_t raw_status_register;
    };

    uint8_t sequencer_mode : 1;
    uint8_t frame_interrupt_inhibit : 1;

    int64_t clock_counter;
};

static inline bool pulse_muted(const PulseChannel* pulse) {
    return pulse->timer_period < 0x08 || (!pulse->sweep_negate && pulse->sweep_target_period > 0x7FF);
}

static inline uint16_t pulse_get_target_period(const PulseChannel* pulse, bool is_pulse_1) {
    const uint16_t period_change = pulse->timer_period >> pulse->sweep_shift;
    return pulse->sweep_negate ? 
        pulse->timer_period - period_change - is_pulse_1
        : pulse->timer_period + period_change;
}

static void pulse_clock_envelope(PulseChannel* pulse) {
    if (--pulse->envelope_counter == 0) {
        pulse->envelope_counter = pulse->volume_envelope_period + 1;
        if (pulse->envelope_volume > 0) {
            pulse->envelope_volume--;
        } else if (pulse->loop) {
            pulse->envelope_volume = 0x0F;
        }
    }
}

static void pulse_clock_sweep(PulseChannel* pulse, bool is_pulse_1) {
    pulse->sweep_target_period = pulse_get_target_period(pulse, is_pulse_1);
    
    if (--pulse->sweep_counter == 0) {
        pulse->sweep_counter = pulse->sweep_period + 1;
        
        if (pulse->sweep_enabled && !pulse_muted(pulse) && (pulse->sweep_shift > 0)) {
            pulse->timer_period = pulse->sweep_target_period;
        }
    }
}

static void pulse_clock_length_counter(PulseChannel* pulse) {
    if (!pulse->loop && pulse->length_counter > 0) {
        pulse->length_counter--;
    }
}

static void pulse_clock_timer(PulseChannel* pulse) {
    if (pulse->timer_counter > 0) {
        pulse->timer_counter--;
    }

    if (pulse->timer_counter == 0) {
        pulse->sequence_pos--;
        pulse->timer_counter = pulse->timer_period + 1;
    }
}

static uint8_t pulse_get_output(PulseChannel* pulse) {
    if (pulse_muted(pulse) || pulse->length_counter == 0) return 0;

    const uint8_t sequence = PULSE_SEQ[pulse->duty_cycle];
    const uint8_t value = (sequence >> pulse->sequence_pos) & 0b00000001;
    const uint8_t volume = pulse->constant_volume ? pulse->volume_envelope_period : pulse->envelope_volume;
    return value * volume;
}

static void triangle_clock_linear_counter(TriangleChannel* triangle) {
    if (triangle->halt) {
        triangle->linear_counter = triangle->linear_counter_load;
    } else if (triangle->linear_counter > 0) {
        triangle->linear_counter--;
    }

    if (!triangle->loop) {
        triangle->halt = false;
    }
}

static void triangle_clock_length_counter(TriangleChannel* triangle) {
    if (!triangle->loop && triangle->length_counter > 0) {
        triangle->length_counter--;
    }
}

static void triangle_clock_timer(TriangleChannel* triangle) {
    // NOTE: Triangle ticks at CPU clock rate (APU clock x2)
    for (int i = 0; i < 2; i++) {
        if (triangle->length_counter > 0 && triangle->linear_counter > 0) {

            if (triangle->timer_counter > 0) {
                triangle->timer_counter--;
            }

            if (triangle->timer_counter == 0) {
                triangle->sequence--;
                triangle->timer_counter = triangle->timer_period + 1;
            }
        }
    }
}

static uint8_t triangle_get_output(TriangleChannel* triangle) {
    return TRIANGLE_SEQ[triangle->sequence];
}

static void noise_clock_envelope(NoiseChannel* noise) {
    if (--noise->envelope_counter == 0) {
        noise->envelope_counter = noise->volume_envelope_period + 1;
        if (noise->envelope_volume > 0) {
            noise->envelope_volume--;
        } else if (noise->loop) {
            noise->envelope_volume = 0x0F;
        }
    }
}

static void noise_clock_length_counter(NoiseChannel* noise) {
    if (!noise->loop && noise->length_counter > 0) {
        noise->length_counter--;
    }
}

static void noise_clock_timer(NoiseChannel* noise) {
    if (noise->timer_counter > 0) {
        noise->timer_counter--;
    }

    if (noise->timer_counter == 0) {
        const uint8_t mode_bit_index = noise->mode ? 6 : 1;
        const uint8_t mode_bit = (uint8_t)noise->shift_register >> mode_bit_index;
        const uint8_t feedback_bit = (noise->shift_register ^ mode_bit) & 1;

        noise->shift_register >>= 1;
        noise->shift_register |= (feedback_bit << 14);

        // TODO: PAL support
        noise->timer_counter = NOISE_PERIOD_NTSC[noise->period];
    }
}

static uint8_t noise_get_output(NoiseChannel* noise) {
    if (noise->length_counter == 0 || noise->shift_register & 1) {
        return 0;
    }

    const uint8_t volume = noise->constant_volume ? noise->volume_envelope_period : noise->envelope_volume;
    return volume;
}

static void dmc_load_sample(DPCMChannel* dmc) {
    dmc->current_address = 0xC000 + (dmc->sample_address << 6);
    dmc->bytes_remaining = (dmc->sample_length << 4) + 1;
}

static bool dmc_try_fill_buffer(DPCMChannel* dmc) {
    bool interrupt = false;
    
    if (!dmc->buffer_filled && dmc->bytes_remaining != 0) {
        // TODO: Accurate CPU stall? From nesdev:
        // "The CPU is stalled for 1-4 CPU cycles to read a sample byte."

        if (dmc->reader != NULL) {
            dmc->sample_buffer = dmc->reader(dmc->reader_data, dmc->current_address);
        }
        dmc->buffer_filled = true;

        if (dmc->current_address == 0xFFFF) {
            dmc->current_address = 0x8000;
        } else {
            dmc->current_address++;
        }

        dmc->bytes_remaining--;
        if (dmc->bytes_remaining == 0) {
            if (dmc->loop) {
                dmc_load_sample(dmc);
            } else if (dmc->enable_irq) {
                interrupt = true;
            }
        }
    }

    return interrupt;
}

static bool dmc_clock_timer(DPCMChannel* dmc) {
    bool interrupt = false;

    // NOTE: DMC ticks at CPU rate
    for (int i = 0; i < 2; i++) {
        if (dmc->timer_counter > 0) {
            dmc->timer_counter--;
        }

        // DMA reader
        interrupt |= dmc_try_fill_buffer(dmc);

        // Output unit
        if (dmc->timer_counter == 0) {
            if (dmc->playing) {
                if (dmc->shift_register & 1) {
                    if (dmc->output_level <= 125) {
                        dmc->output_level += 2;
                    }
                } else if (dmc->output_level >= 2) {
                    dmc->output_level -= 2;
                }
            }

            dmc->shift_register >>= 1;

            if (dmc->bits_remaining > 0) {
                dmc->bits_remaining--;
            }

            if (dmc->bits_remaining == 0) {
                dmc->bits_remaining = 8;
                if (!dmc->buffer_filled) {
                    dmc->playing = false;
                } else {
                    dmc->playing = true;
                    dmc->shift_register = dmc->sample_buffer;
                    dmc->buffer_filled = false;
                }
            }

            // TODO: PAL support
            dmc->timer_counter = DMC_RATE_NTSC[dmc->sample_rate];
        }
    }

    return interrupt;
}

static void apu_clock_quarter_frame(FamApu* apu) {
    pulse_clock_envelope(apu->pulse);
    pulse_clock_envelope(apu->pulse + 1);

    triangle_clock_linear_counter(&apu->triangle);

    noise_clock_envelope(&apu->noise);
}

static void apu_clock_half_frame(FamApu* apu) {
    pulse_clock_sweep(apu->pulse, true);
    pulse_clock_sweep(apu->pulse + 1, false);

    pulse_clock_length_counter(apu->pulse);
    pulse_clock_length_counter(apu->pulse + 1);

    triangle_clock_length_counter(&apu->triangle);

    noise_clock_length_counter(&apu->noise);
}

static void apu_clock_frame(FamApu* apu) {
    if (apu->sequencer_mode == 0 && !apu->frame_interrupt_inhibit) {
        // TODO: Do we want to fire an actual mock interrupt? (As a callback?)
        apu->status.frame_interrupt = 1;
    }
    apu->clock_counter = 0;
}

static void apu_write_pulse_register(FamApu* apu, int index, int offset, uint8_t data) {
    PulseChannel* pulse = apu->pulse + index;
    pulse->raw_registers[offset] = data;

    const bool enabled = index == 1 ? apu->status.enable_pulse2 : apu->status.enable_pulse1;

    switch (offset) {
        case 0:
            // Nesdev: The duty cycle is changed (see table below), but the sequencer's current position isn't affected.
            // So I guess nothing happens? Double check if sounds weird
            break;
        case 1:
            pulse->sweep_target_period = pulse_get_target_period(pulse, index == 0);
            pulse->sweep_counter = pulse->sweep_period + 1;
            break;
        case 2:
            pulse->sweep_target_period = pulse_get_target_period(pulse, index == 0);
            break;
        case 3:
            // Nesdev: The sequencer is immediately restarted at the first value of the current sequence. 
            // The envelope is also restarted. The period divider is not reset.
            pulse->sweep_target_period = pulse_get_target_period(pulse, index == 0);
            pulse->sequence_pos = 0;

            pulse->envelope_counter = pulse->volume_envelope_period + 1;
            pulse->envelope_volume = 0x0F;

            if (enabled) {
                pulse->length_counter = LENGTH_TABLE[pulse->length_counter_load];
            }
            break;
        default:
            break;
    }
}

static void apu_write_triangle_register(FamApu* apu, int offset, uint8_t data) {
    TriangleChannel* triangle = &apu->triangle;
    triangle->raw_registers[offset] = data;
    
    switch (offset) {
        case 0:
        case 1:
        case 2:
            break;
        case 3:
            // Nesdev: Sets the linear counter reload flag
            // AKA halt
            triangle->halt = true;

            if (apu->status.enable_triangle) {
                triangle->length_counter = LENGTH_TABLE[triangle->length_counter_load];
            }
            break;
        default:
            break;
    }
}

static void apu_write_noise_register(FamApu* apu, int offset, uint8_t data) {
    NoiseChannel* noise = &apu->noise;
    noise->raw_registers[offset] = data;

    switch (offset) {
        case 0:
        case 1:
        case 2:
            break;
        case 3:
            noise->envelope_counter = noise->volume_envelope_period + 1;
            noise->envelope_volume = 0x0F;

            if (apu->status.enable_noise) {
                noise->length_counter = LENGTH_TABLE[noise->length_counter_load];
            }
            break;
        default:
            break;
    }
}

static void apu_write_dmc_register(FamApu* apu, int offset, uint8_t data) {
    DPCMChannel* dmc = &apu->dmc;
    dmc->raw_registers[offset] = data;

    switch (offset) {
        case 0:
            if (!dmc->enable_irq) {
                apu->status.dmc_interrupt = false;
            }
            break;
        case 1:
            dmc->output_level = dmc->direct_load;
            break;
        case 2:
        case 3:
        default:
            break;
    }
}

FamResult fam_apu_init(FamApu** out_apu) {
    if (out_apu == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }
    FamApu* apu = (FamApu*)calloc(1, sizeof(FamApu));
    if (apu == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }

    // TODO: Should these be in their own function?
    apu->noise.shift_register = 1;
    // TODO: PAL support
    apu->noise.timer_counter = NOISE_PERIOD_NTSC[0];

    *out_apu = apu;
    return FAM_SUCCESS;
}

void fam_apu_free(FamApu* apu) {
    free(apu);
}

FamResult fam_apu_write_register(FamApu* apu, uint16_t reg, uint8_t data) {
    switch (reg) {
        case FAM_REGISTER_PULSE1_DUTY:
        case FAM_REGISTER_PULSE1_SWEEP:
        case FAM_REGISTER_PULSE1_TIMER_LO:
        case FAM_REGISTER_PULSE1_TIMER_HI: {
            int offset = (int)reg - FAM_REGISTER_PULSE1_DUTY;
            apu_write_pulse_register(apu, 0, offset, data);
            break;
        }
        case FAM_REGISTER_PULSE2_DUTY:
        case FAM_REGISTER_PULSE2_SWEEP:
        case FAM_REGISTER_PULSE2_TIMER_LO:
        case FAM_REGISTER_PULSE2_TIMER_HI: {
            int offset = (int)reg - FAM_REGISTER_PULSE2_DUTY;
            apu_write_pulse_register(apu, 1, offset, data);
            break;
        }
        case FAM_REGISTER_TRIANGLE_COUNTER:
        case FAM_REGISTER_TRIANGLE_UNUSED:
        case FAM_REGISTER_TRIANGLE_TIMER_LO:
        case FAM_REGISTER_TRIANGLE_TIMER_HI: {
            int offset = (int)reg - FAM_REGISTER_TRIANGLE_COUNTER;
            apu_write_triangle_register(apu, offset, data);
            break;
        }
        case FAM_REGISTER_NOISE_ENVELOPE:
        case FAM_REGISTER_NOISE_UNUSED:
        case FAM_REGISTER_NOISE_PERIOD:
        case FAM_REGISTER_NOISE_LOAD: {
            int offset = (int)reg - FAM_REGISTER_NOISE_ENVELOPE;
            apu_write_noise_register(apu, offset, data);
            break;
        }
        case FAM_REGISTER_DMC_FLAGS:
        case FAM_REGISTER_DMC_LOAD:
        case FAM_REGISTER_DMC_ADDRESS:
        case FAM_REGISTER_DMC_LENGTH: {
            int offset = (int)reg - FAM_REGISTER_DMC_FLAGS;
            apu_write_dmc_register(apu, offset, data);
            break;
        }
        case FAM_REGISTER_STATUS: {
            // Only set first 5 bits
            apu->raw_status_register = (apu->raw_status_register & 0b11100000) | (data & 0b00011111);
            // Always clear DMC IRQ
            apu->status.dmc_interrupt = false;
            if (!apu->status.enable_pulse1) {
                apu->pulse->length_counter = 0;
            }
            if (!apu->status.enable_pulse2) {
                (apu->pulse + 1)->length_counter = 0;
            }
            if (!apu->status.enable_triangle) {
                apu->triangle.length_counter = 0;
            }
            if (!apu->status.enable_noise) {
                apu->noise.length_counter = 0;
            }
            if (!apu->status.enable_dmc) {
                apu->dmc.bytes_remaining = 0;
            } else if (apu->dmc.bytes_remaining == 0) {
                dmc_load_sample(&apu->dmc);
                apu->status.dmc_interrupt |= dmc_try_fill_buffer(&apu->dmc);
            }
            break;
        }
        case FAM_REGISTER_FRAME_COUNTER: {
            apu->clock_counter = 0;
            apu->sequencer_mode = data >> 7;
            apu->frame_interrupt_inhibit = (data >> 6) & 1;
            // Nesdev: Interrupt inhibit flag. If set, the frame interrupt flag is cleared, otherwise it is unaffected.
            if (apu->frame_interrupt_inhibit) {
                apu->status.frame_interrupt = 0;
            }
            // Nesdev: If the mode flag is set, then both "quarter frame" and "half frame" signals are also generated.
            if (apu->sequencer_mode == 1) {
                apu_clock_quarter_frame(apu);
                apu_clock_half_frame(apu);
            }
            break;
        }
        default:
            return FAM_ERROR_INVALID_ARGUMENT;
    }

    return FAM_SUCCESS;
}

FamResult fam_apu_read_register(FamApu* apu, uint16_t reg, uint8_t* out_data) {
    switch (reg) {
        case FAM_REGISTER_PULSE1_DUTY:
        case FAM_REGISTER_PULSE1_SWEEP:
        case FAM_REGISTER_PULSE1_TIMER_LO:
        case FAM_REGISTER_PULSE1_TIMER_HI:
        case FAM_REGISTER_PULSE2_DUTY:
        case FAM_REGISTER_PULSE2_SWEEP:
        case FAM_REGISTER_PULSE2_TIMER_LO:
        case FAM_REGISTER_PULSE2_TIMER_HI:
        case FAM_REGISTER_TRIANGLE_COUNTER:
        case FAM_REGISTER_TRIANGLE_UNUSED:
        case FAM_REGISTER_TRIANGLE_TIMER_LO:
        case FAM_REGISTER_TRIANGLE_TIMER_HI:
        case FAM_REGISTER_NOISE_ENVELOPE:
        case FAM_REGISTER_NOISE_UNUSED:
        case FAM_REGISTER_NOISE_PERIOD:
        case FAM_REGISTER_NOISE_LOAD:
            return FAM_ERROR_WRITE_ONLY;
        case FAM_REGISTER_STATUS: {
            // Keep bit 5 as it was (Open bus approximation)
            *out_data = (*out_data & 0b00100000) | (apu->raw_status_register & 0b11011111);
            if (apu->pulse[0].length_counter == 0) *out_data &= 0b11111110;
            if (apu->pulse[1].length_counter == 0) *out_data &= 0b11111101;
            if (apu->triangle.length_counter == 0) *out_data &= 0b11111011;
            if (apu->noise.length_counter == 0) *out_data &= 0b11110111;
            if (apu->dmc.bytes_remaining == 0) *out_data &= 0b11101111;
            apu->status.frame_interrupt = 0;
            break;
        }
        case FAM_REGISTER_FRAME_COUNTER:
            return FAM_ERROR_WRITE_ONLY;
        default:
            return FAM_ERROR_INVALID_ARGUMENT;
    }

    return FAM_SUCCESS;
}

void fam_apu_set_dmc_reader(FamApu* apu, FamDmcReadFn reader, void* user_data) {
    apu->dmc.reader = reader;
    apu->dmc.reader_data = user_data;
}

void fam_apu_clock(FamApu* apu) {
    apu->clock_counter++;

    if (apu->clock_counter == QUARTER_FRAME_CLOCK) {
        apu_clock_quarter_frame(apu);
    } else if (apu->clock_counter == HALF_FRAME_CLOCK) {
        apu_clock_quarter_frame(apu);
        apu_clock_half_frame(apu);
    } else if (apu->clock_counter == THREEQUARTERS_FRAME_CLOCK) {
        apu_clock_quarter_frame(apu);
    } else if ((apu->sequencer_mode == 0 && apu->clock_counter == FRAME_CLOCK) 
        || (apu->sequencer_mode == 1 && apu->clock_counter == FRAME_CLOCK_MODE1)) {
        apu_clock_quarter_frame(apu);
        apu_clock_half_frame(apu);
        apu_clock_frame(apu);
    }

    pulse_clock_timer(apu->pulse);
    pulse_clock_timer(apu->pulse + 1);
    triangle_clock_timer(&apu->triangle);
    noise_clock_timer(&apu->noise);
    apu->status.dmc_interrupt |= dmc_clock_timer(&apu->dmc);
}

void fam_apu_get_sample(FamApu* apu, void* out_sample) {
    float pulse_out = 0.0f;
    uint8_t pulse_sum = pulse_get_output(apu->pulse);
    pulse_sum += pulse_get_output(apu->pulse + 1);

    if (pulse_sum > 0) {
        pulse_out = 95.52f / (8128.0f / pulse_sum + 100);
    }

    float tnd_out = 0.0f;
    uint8_t triangle = triangle_get_output(&apu->triangle);
    uint8_t noise = noise_get_output(&apu->noise);
    uint8_t dmc = apu->dmc.output_level;

    if (triangle != 0 || noise != 0 || dmc != 0) {
        tnd_out = 159.79f / (1 / ((float)triangle / 8227 + (float)noise / 12241 + (float)dmc / 22638) + 100);
    }

    float mix = pulse_out + tnd_out;

    // TODO: High pass filter

    // TODO: Support other output formats
    *(float*)out_sample = mix;
}

double fam_apu_get_freq(FamApu* apu) {
    // TODO: Add PAL support
    return (double)NES_CPU_FREQ_NTSC / 2.0;
}