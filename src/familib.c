#include "familib.h"
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

static const uint8_t LENGTH_TABLE[0x20] = {
    10, 254, 20, 2, 40, 4, 80, 6,
    160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22,
    192, 24, 72, 26, 16, 28, 32, 30
};

typedef struct fam_PulseChannel {
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
} fam_PulseChannel;

typedef struct fam_TriangleChannel {
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

} fam_TriangleChannel;

typedef struct fam_StatusRegister {
    uint8_t enable_pulse1 : 1;
    uint8_t enable_pulse2 : 1;
    uint8_t enable_triangle : 1;
    uint8_t enable_noise : 1;
    uint8_t enable_dmc : 1;
    uint8_t unused : 1; // NOTE: Open bus bit, not implemented
    uint8_t frame_interrupt : 1;
    uint8_t dmc_interrupt : 1;
} fam_StatusRegister;

struct fam_Apu {
    fam_PulseChannel pulse[2];
    fam_TriangleChannel triangle;
    // fam_NoiseChannel noise;
    // fam_DmcChannel dmc;
    union {
        fam_StatusRegister status;
        uint8_t raw_status_register;
    };

    uint8_t sequencer_mode : 1;
    uint8_t frame_interrupt_inhibit : 1;

    int64_t clock_counter;
};

static inline bool fam__apu_pulse_muted(const fam_PulseChannel* pulse) {
    return pulse->timer_period < 0x08 || (!pulse->sweep_negate && pulse->sweep_target_period > 0x7FF);
}

static inline uint16_t fam__apu_pulse_get_target_period(const fam_PulseChannel* pulse, bool is_pulse_1) {
    const uint16_t period_change = pulse->timer_period >> pulse->sweep_shift;
    return pulse->sweep_negate ? 
        pulse->timer_period - period_change - is_pulse_1
        : pulse->timer_period + period_change;
}

static void fam__pulse_channel_clock_envelope(fam_PulseChannel* pulse) {
    if (--pulse->envelope_counter == 0) {
        pulse->envelope_counter = pulse->volume_envelope_period + 1;
        if (pulse->envelope_volume > 0) {
            pulse->envelope_volume--;
        } else if (pulse->loop) {
            pulse->envelope_volume = 0x0F;
        }
    }
}

static void fam__pulse_channel_clock_sweep(fam_PulseChannel* pulse, bool is_pulse_1) {
    pulse->sweep_target_period = fam__apu_pulse_get_target_period(pulse, is_pulse_1);
    
    if (--pulse->sweep_counter == 0) {
        pulse->sweep_counter = pulse->sweep_period + 1;
        
        if (pulse->sweep_enabled && !fam__apu_pulse_muted(pulse) && (pulse->sweep_shift > 0)) {
            pulse->timer_period = pulse->sweep_target_period;
        }
    }
}

static void fam__pulse_channel_clock_length_counter(fam_PulseChannel* pulse) {
    if (!pulse->loop && pulse->length_counter > 0) {
        pulse->length_counter--;
    }
}

static void fam__triangle_channel_clock_linear_counter(fam_TriangleChannel* triangle) {
    if (triangle->halt) {
        triangle->linear_counter = triangle->linear_counter_load;
    } else if (triangle->linear_counter > 0) {
        triangle->linear_counter--;
    }

    if (!triangle->loop) {
        triangle->halt = false;
    }
}

static void fam__triangle_channel_clock_length_counter(fam_TriangleChannel* triangle) {
    if (!triangle->loop && triangle->length_counter > 0) {
        triangle->length_counter--;
    }
}

static void fam__apu_clock_quarter_frame(fam_Apu* apu) {
    fam__pulse_channel_clock_envelope(apu->pulse);
    fam__pulse_channel_clock_envelope(apu->pulse + 1);

    fam__triangle_channel_clock_linear_counter(&apu->triangle);
}

static void fam__apu_clock_half_frame(fam_Apu* apu) {
    fam__pulse_channel_clock_sweep(apu->pulse, true);
    fam__pulse_channel_clock_sweep(apu->pulse + 1, false);

    fam__pulse_channel_clock_length_counter(apu->pulse);
    fam__pulse_channel_clock_length_counter(apu->pulse + 1);

    fam__triangle_channel_clock_length_counter(&apu->triangle);
}

static void fam__apu_clock_frame(fam_Apu* apu) {
    if (apu->sequencer_mode == 0 && !apu->frame_interrupt_inhibit) {
        // TODO: Do we want to fire an actual mock interrupt? (As a callback?)
        apu->status.frame_interrupt = 1;
    }
    apu->clock_counter = 0;
}

static uint8_t fam__apu_process_pulse(fam_PulseChannel* pulse) {
    pulse->timer_counter--;
    if (pulse->timer_counter == 0xFFFF) {
        pulse->sequence_pos--;
        pulse->timer_counter = pulse->timer_period + 1;
    }

    if (fam__apu_pulse_muted(pulse) || pulse->length_counter == 0) return 0;

    const uint8_t sequence = PULSE_SEQ[pulse->duty_cycle];
    const uint8_t value = (sequence >> pulse->sequence_pos) & 0b00000001;
    const uint8_t volume = pulse->constant_volume ? pulse->volume_envelope_period : pulse->envelope_volume;
    return value * volume;
}

static void fam__apu_write_pulse_register(fam_Apu* apu, int index, int offset, uint8_t data) {
    fam_PulseChannel* pulse = apu->pulse + index;
    pulse->raw_registers[offset] = data;

    const bool enabled = index == 1 ? apu->status.enable_pulse2 : apu->status.enable_pulse1;

    switch (offset) {
        case 0:
            // Nesdev: The duty cycle is changed (see table below), but the sequencer's current position isn't affected.
            // So I guess nothing happens? Double check if sounds weird
            break;
        case 1:
            pulse->sweep_target_period = fam__apu_pulse_get_target_period(pulse, index == 0);
            pulse->sweep_counter = pulse->sweep_period + 1;
            break;
        case 2:
            pulse->sweep_target_period = fam__apu_pulse_get_target_period(pulse, index == 0);
            break;
        case 3:
            // Nesdev: The sequencer is immediately restarted at the first value of the current sequence. The envelope is also restarted. The period divider is not reset.
            pulse->sweep_target_period = fam__apu_pulse_get_target_period(pulse, index == 0);
            pulse->sequence_pos = 0;
            pulse->timer_counter = pulse->timer_period + 1;

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

static uint8_t fam__apu_process_triangle(fam_TriangleChannel* triangle) {
    // NOTE: Triangle ticks at CPU clock rate (APU clock x2)
    for (int i = 0; i < 2; i++) {
        if (triangle->length_counter > 0 && triangle->linear_counter > 0) {
            triangle->timer_counter--;
            if (triangle->timer_counter == 0xFFFF) {
                triangle->sequence--;
                triangle->timer_counter = triangle->timer_period + 1;
            }
        }
    }

    if (triangle->length_counter == 0 || triangle->linear_counter == 0) {
        return 0;
    }

    const uint8_t value = TRIANGLE_SEQ[triangle->sequence];
    return value;
}

static void fam__apu_write_triangle_register(fam_Apu* apu, int offset, uint8_t data) {
    fam_TriangleChannel* triangle = &apu->triangle;
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

            triangle->timer_counter = triangle->timer_period + 1;

            if (apu->status.enable_triangle) {
                triangle->length_counter = LENGTH_TABLE[triangle->length_counter_load];
            }
            break;
        default:
            break;
    }
}

fam_Apu* fam_apu_init() {
    fam_Apu* result = (fam_Apu*)calloc(1, sizeof(fam_Apu));
    return result;
}

void fam_apu_free(fam_Apu* apu) {
    free(apu);
}

fam_Result fam_apu_write_register(fam_Apu* apu, fam_Register reg, uint8_t data) {
    switch (reg) {
        case FAM_REGISTER_PULSE1_DUTY:
        case FAM_REGISTER_PULSE1_SWEEP:
        case FAM_REGISTER_PULSE1_TIMER_LO:
        case FAM_REGISTER_PULSE1_TIMER_HI: {
            int offset = (int)reg - FAM_REGISTER_PULSE1_DUTY;
            fam__apu_write_pulse_register(apu, 0, offset, data);
            break;
        }
        case FAM_REGISTER_PULSE2_DUTY:
        case FAM_REGISTER_PULSE2_SWEEP:
        case FAM_REGISTER_PULSE2_TIMER_LO:
        case FAM_REGISTER_PULSE2_TIMER_HI: {
            int offset = (int)reg - FAM_REGISTER_PULSE2_DUTY;
            fam__apu_write_pulse_register(apu, 1, offset, data);
            break;
        }
        case FAM_REGISTER_TRIANGLE_COUNTER:
        case FAM_REGISTER_TRIANGLE_UNUSED:
        case FAM_REGISTER_TRIANGLE_TIMER_LO:
        case FAM_REGISTER_TRIANGLE_TIMER_HI: {
            int offset = (int)reg - FAM_REGISTER_TRIANGLE_COUNTER;
            fam__apu_write_triangle_register(apu, offset, data);
            break;
        }
        case FAM_REGISTER_STATUS: {
            // Only set first 5 bits
            apu->raw_status_register = (apu->raw_status_register & 0b11100000) | (data & 0b00011111);
            if (!apu->status.enable_pulse1) {
                apu->pulse->length_counter = 0;
            }
            if (!apu->status.enable_pulse2) {
                (apu->pulse + 1)->length_counter = 0;
            }
            if (!apu->status.enable_triangle) {
                apu->triangle.length_counter = 0;
            }
            // TODO: Handle other side-effects
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
                fam__apu_clock_quarter_frame(apu);
                fam__apu_clock_half_frame(apu);
            }
            break;
        }
        default:
            return FAM_ERROR_INVALID_REGISTER;
    }

    return FAM_SUCCESS;
}

fam_Result fam_apu_read_register(fam_Apu* apu, fam_Register reg, uint8_t* out_data) {
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
            return FAM_ERROR_WRITE_ONLY;
        case FAM_REGISTER_STATUS: {
            // Keep bit 5 as it was
            *out_data = (*out_data & 0b00100000) | (apu->raw_status_register & 0b11011111);
            if (apu->pulse[0].length_counter == 0) *out_data &= 0b11111110;
            if (apu->pulse[1].length_counter == 0) *out_data &= 0b11111101;
            if (apu->triangle.length_counter == 0) *out_data &= 0b11111011;
            apu->status.frame_interrupt = 0;
            break;
        }
        case FAM_REGISTER_FRAME_COUNTER:
            return FAM_ERROR_WRITE_ONLY;
        default:
            return FAM_ERROR_INVALID_REGISTER;
    }

    return FAM_SUCCESS;
}

void fam_apu_clock(fam_Apu* apu, float* out_sample) {
    apu->clock_counter++;

    if (apu->clock_counter == QUARTER_FRAME_CLOCK) {
        fam__apu_clock_quarter_frame(apu);
    } else if (apu->clock_counter == HALF_FRAME_CLOCK) {
        fam__apu_clock_quarter_frame(apu);
        fam__apu_clock_half_frame(apu);
    } else if (apu->clock_counter == THREEQUARTERS_FRAME_CLOCK) {
        fam__apu_clock_quarter_frame(apu);
    } else if ((apu->sequencer_mode == 0 && apu->clock_counter == FRAME_CLOCK) 
        || (apu->sequencer_mode == 1 && apu->clock_counter == FRAME_CLOCK_MODE1)) {
        fam__apu_clock_quarter_frame(apu);
        fam__apu_clock_half_frame(apu);
        fam__apu_clock_frame(apu);
    }

    float pulse_out = 0.0f;
    uint8_t pulse_sum = fam__apu_process_pulse(apu->pulse);
    pulse_sum += fam__apu_process_pulse(apu->pulse + 1);

    if (pulse_sum > 0) {
        pulse_out = 95.52f / (8128.0f / pulse_sum + 100);
    }

    float tnd_out = 0.0f;
    uint8_t triangle = fam__apu_process_triangle(&apu->triangle);
    uint8_t noise = 0;
    uint8_t dmc = 0;

    if (triangle != 0 || noise != 0 || dmc != 0) {
        tnd_out = 159.79f / (1 / ((float)triangle / 8227 + (float)noise / 12241 + (float)dmc / 22638) + 100);
    }

    float mix = pulse_out + tnd_out;

    // TODO: High pass filter

    if (out_sample != NULL) {
        *out_sample = mix;
    }
}

double fam_apu_get_freq(fam_Apu* apu) {
    // TODO: Add PAL support
    return (double)NES_CPU_FREQ_NTSC / 2.0;
}