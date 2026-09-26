#include <fam/apu.h>
#include <string.h>
#include <stdalign.h>

typedef enum {
    QUARTER_FRAME_CLOCK = 0,
    HALF_FRAME_CLOCK,
    THREEQUARTERS_FRAME_CLOCK,
    FRAME_CLOCK,
    FRAME_CLOCK_MODE1,

    FRAME_CLOCK_COUNT,
} FrameSequencerStep;

// Tables indexed by region
static const double NES_CPU_FREQ[2] = { 1789773.0, 1662607.0 };
static const double NES_CPU_CYCLES_PER_FRAME[2] = { 29780.5, 33247.5 }; // 60.099 Hz / 50.007 Hz
// NOTE: Values are one more than written on Nesdev,
// because my APU clock function increments the counter at the beginning, 
// so at cycle 0 it's set to 1 immediately before comparing
static const uint16_t FRAME_SEQ_CLOCK[2][FRAME_CLOCK_COUNT] = {
    { 3729, 7457, 11186, 14915, 18641 }, // NTSC
    { 4157, 8314, 12470, 16627, 20783 } // PAL
};
// NOTE: These noise period values are half what is written on Nesdev, 
// because they're APU cycles instead of CPU cycles
static const uint16_t NOISE_PERIOD[2][16] = {
    { 2, 4, 8, 16, 32, 48, 64, 80, 101, 127, 190, 254, 381, 508, 1017, 2034 }, // NTSC
    { 2, 4, 7, 15, 30, 44, 59, 74, 94, 118, 177, 236, 354, 472, 945, 1889 }    // PAL
};

static const uint16_t DMC_RATE[2][16] = {
    { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 }, // NTSC
    { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 }   // PAL
};


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

// Mixer output value tables
static const float PULSE_MIX_TABLE[31] = {
    0.000000f, 0.011609f, 0.022939f, 0.034001f, 0.044803f, 0.055355f, 0.065665f, 0.075741f, 
    0.085591f, 0.095224f, 0.104645f, 0.113862f, 0.122882f, 0.131710f, 0.140353f, 0.148816f, 
    0.157105f, 0.165226f, 0.173183f, 0.180981f, 0.188626f, 0.196120f, 0.203470f, 0.210679f, 
    0.217751f, 0.224689f, 0.231499f, 0.238182f, 0.244744f, 0.251186f, 0.257513f 
};

static const float TND_MIX_TABLE[203] = {
    0.000000f, 0.006700f, 0.013345f, 0.019936f, 0.026474f, 0.032959f, 0.039393f, 0.045775f, 
    0.052106f, 0.058386f, 0.064618f, 0.070800f, 0.076934f, 0.083020f, 0.089058f, 0.095050f, 
    0.100996f, 0.106896f, 0.112751f, 0.118561f, 0.124327f, 0.130049f, 0.135728f, 0.141365f, 
    0.146959f, 0.152512f, 0.158024f, 0.163494f, 0.168925f, 0.174315f, 0.179666f, 0.184978f, 
    0.190252f, 0.195487f, 0.200684f, 0.205845f, 0.210968f, 0.216054f, 0.221105f, 0.226120f, 
    0.231099f, 0.236043f, 0.240953f, 0.245828f, 0.250669f, 0.255477f, 0.260252f, 0.264993f, 
    0.269702f, 0.274379f, 0.279024f, 0.283638f, 0.288220f, 0.292771f, 0.297292f, 0.301782f, 
    0.306242f, 0.310673f, 0.315074f, 0.319446f, 0.323789f, 0.328104f, 0.332390f, 0.336649f, 
    0.340879f, 0.345083f, 0.349259f, 0.353408f, 0.357530f, 0.361626f, 0.365696f, 0.369740f, 
    0.373759f, 0.377752f, 0.381720f, 0.385662f, 0.389581f, 0.393474f, 0.397344f, 0.401189f, 
    0.405011f, 0.408809f, 0.412584f, 0.416335f, 0.420064f, 0.423770f, 0.427454f, 0.431115f, 
    0.434754f, 0.438371f, 0.441966f, 0.445540f, 0.449093f, 0.452625f, 0.456135f, 0.459625f, 
    0.463094f, 0.466543f, 0.469972f, 0.473380f, 0.476769f, 0.480138f, 0.483488f, 0.486818f, 
    0.490129f, 0.493421f, 0.496694f, 0.499948f, 0.503184f, 0.506402f, 0.509601f, 0.512782f, 
    0.515946f, 0.519091f, 0.522219f, 0.525330f, 0.528423f, 0.531499f, 0.534558f, 0.537601f, 
    0.540626f, 0.543635f, 0.546627f, 0.549603f, 0.552563f, 0.555507f, 0.558434f, 0.561346f, 
    0.564242f, 0.567123f, 0.569988f, 0.572838f, 0.575673f, 0.578493f, 0.581298f, 0.584088f, 
    0.586863f, 0.589623f, 0.592370f, 0.595101f, 0.597819f, 0.600522f, 0.603212f, 0.605887f, 
    0.608549f, 0.611197f, 0.613831f, 0.616452f, 0.619059f, 0.621653f, 0.624234f, 0.626802f, 
    0.629357f, 0.631899f, 0.634428f, 0.636944f, 0.639448f, 0.641939f, 0.644418f, 0.646885f, 
    0.649339f, 0.651781f, 0.654212f, 0.656630f, 0.659036f, 0.661431f, 0.663813f, 0.666185f, 
    0.668544f, 0.670893f, 0.673229f, 0.675555f, 0.677869f, 0.680173f, 0.682465f, 0.684746f, 
    0.687017f, 0.689276f, 0.691525f, 0.693763f, 0.695991f, 0.698208f, 0.700415f, 0.702611f, 
    0.704797f, 0.706973f, 0.709139f, 0.711294f, 0.713440f, 0.715576f, 0.717702f, 0.719818f, 
    0.721924f, 0.724021f, 0.726108f, 0.728186f, 0.730254f, 0.732313f, 0.734362f, 0.736402f, 
    0.738433f, 0.740455f, 0.742468f, 
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
    uint8_t region : 1;

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

static void noise_clock_timer(NoiseChannel* noise, uint8_t region) {
    if (noise->timer_counter > 0) {
        noise->timer_counter--;
    }

    if (noise->timer_counter == 0) {
        const uint8_t mode_bit_index = noise->mode ? 6 : 1;
        const uint8_t mode_bit = (uint8_t)noise->shift_register >> mode_bit_index;
        const uint8_t feedback_bit = (noise->shift_register ^ mode_bit) & 1;

        noise->shift_register >>= 1;
        noise->shift_register |= (feedback_bit << 14);

        noise->timer_counter = NOISE_PERIOD[region][noise->period];
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
        // NOTE: On real hardware, the CPU is stalled for 1-4 CPU cycles to read a sample byte
        // There's no CPU in fam, so I'm not modeling this behaviour

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

static bool dmc_clock_timer(DPCMChannel* dmc, uint8_t region) {
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

            dmc->timer_counter = DMC_RATE[region][dmc->sample_rate];
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

size_t fam_apu_get_memory_required(void) {
    return sizeof(FamApu);
}

size_t fam_apu_get_memory_alignment(void) {
    return alignof(FamApu);
}

FamResult fam_apu_init(FamApu** out_apu, void* memory, FamRegion region) {
    if (out_apu == NULL || memory == NULL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    if (region > FAM_REGION_PAL) {
        return FAM_ERROR_INVALID_ARGUMENT;
    }

    FamApu* apu = (FamApu*)memory;
    memset(apu, 0, sizeof(FamApu));

    apu->region = region;

    // TODO: Should these be in their own function?
    apu->noise.shift_register = 1;
    apu->noise.timer_counter = NOISE_PERIOD[region][0];

    *out_apu = apu;
    return FAM_SUCCESS;
}

void fam_apu_shutdown(FamApu* apu) {
    if (apu == NULL) {
        return;
    }

    apu->dmc.reader = NULL;
    apu->dmc.reader_data = NULL;
}

FamRegion fam_apu_get_region(const FamApu* apu) {
    return apu->region;
}

FamResult fam_apu_write_register(FamApu* apu, uint16_t reg, uint8_t data) {
    switch (reg) {
        case FAM_REGISTER_PULSE1_0:
        case FAM_REGISTER_PULSE1_1:
        case FAM_REGISTER_PULSE1_2:
        case FAM_REGISTER_PULSE1_3: {
            int offset = (int)reg - FAM_REGISTER_PULSE1_0;
            apu_write_pulse_register(apu, 0, offset, data);
            break;
        }
        case FAM_REGISTER_PULSE2_0:
        case FAM_REGISTER_PULSE2_1:
        case FAM_REGISTER_PULSE2_2:
        case FAM_REGISTER_PULSE2_3: {
            int offset = (int)reg - FAM_REGISTER_PULSE2_0;
            apu_write_pulse_register(apu, 1, offset, data);
            break;
        }
        case FAM_REGISTER_TRIANGLE_0:
        case FAM_REGISTER_TRIANGLE_1:
        case FAM_REGISTER_TRIANGLE_2:
        case FAM_REGISTER_TRIANGLE_3: {
            int offset = (int)reg - FAM_REGISTER_TRIANGLE_0;
            apu_write_triangle_register(apu, offset, data);
            break;
        }
        case FAM_REGISTER_NOISE_0:
        case FAM_REGISTER_NOISE_1:
        case FAM_REGISTER_NOISE_2:
        case FAM_REGISTER_NOISE_3: {
            int offset = (int)reg - FAM_REGISTER_NOISE_0;
            apu_write_noise_register(apu, offset, data);
            break;
        }
        case FAM_REGISTER_DMC_0:
        case FAM_REGISTER_DMC_1:
        case FAM_REGISTER_DMC_2:
        case FAM_REGISTER_DMC_3: {
            int offset = (int)reg - FAM_REGISTER_DMC_0;
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
            // NOTE: On real hardware, there's a 3-4 cycle delay depending on when the write happened.
            // There's no CPU emulation in fam, so I'm not modeling this behaviour
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
        case FAM_REGISTER_PULSE1_0:
        case FAM_REGISTER_PULSE1_1:
        case FAM_REGISTER_PULSE1_2:
        case FAM_REGISTER_PULSE1_3:
        case FAM_REGISTER_PULSE2_0:
        case FAM_REGISTER_PULSE2_1:
        case FAM_REGISTER_PULSE2_2:
        case FAM_REGISTER_PULSE2_3:
        case FAM_REGISTER_TRIANGLE_0:
        case FAM_REGISTER_TRIANGLE_1:
        case FAM_REGISTER_TRIANGLE_2:
        case FAM_REGISTER_TRIANGLE_3:
        case FAM_REGISTER_NOISE_0:
        case FAM_REGISTER_NOISE_1:
        case FAM_REGISTER_NOISE_2:
        case FAM_REGISTER_NOISE_3:
            return FAM_ERROR_WRITE_ONLY;
        case FAM_REGISTER_STATUS: {
            // NOTE: On real hardware, bit 5 is open bus.
            // I'm not modeling open bus behaviour, so it's always zero
            *out_data = apu->raw_status_register & 0b11011111;
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


    const uint16_t* frame_seq_clock = FRAME_SEQ_CLOCK[apu->region];
    if (apu->clock_counter == frame_seq_clock[QUARTER_FRAME_CLOCK]) {
        apu_clock_quarter_frame(apu);
    } else if (apu->clock_counter == frame_seq_clock[HALF_FRAME_CLOCK]) {
        apu_clock_quarter_frame(apu);
        apu_clock_half_frame(apu);
    } else if (apu->clock_counter == frame_seq_clock[THREEQUARTERS_FRAME_CLOCK]) {
        apu_clock_quarter_frame(apu);
    } else if ((apu->sequencer_mode == 0 && apu->clock_counter == frame_seq_clock[FRAME_CLOCK]) 
        || (apu->sequencer_mode == 1 && apu->clock_counter == frame_seq_clock[FRAME_CLOCK_MODE1])) {
        apu_clock_quarter_frame(apu);
        apu_clock_half_frame(apu);
        apu_clock_frame(apu);
    }

    pulse_clock_timer(apu->pulse);
    pulse_clock_timer(apu->pulse + 1);
    triangle_clock_timer(&apu->triangle);
    noise_clock_timer(&apu->noise, apu->region);
    apu->status.dmc_interrupt |= dmc_clock_timer(&apu->dmc, apu->region);
}

void fam_apu_get_sample(FamApu* apu, float* out_sample) {
    uint8_t pulse_sum = pulse_get_output(apu->pulse);
    pulse_sum += pulse_get_output(apu->pulse + 1);
    
    float pulse_out = PULSE_MIX_TABLE[pulse_sum];

    uint8_t triangle = triangle_get_output(&apu->triangle);
    uint8_t noise = noise_get_output(&apu->noise);
    uint8_t dmc = apu->dmc.output_level;

    float tnd_out = TND_MIX_TABLE[3 * triangle + 2 * noise + dmc];

    float mix = pulse_out + tnd_out;

    // TODO: High pass filter
    // It has to run per fam_apu_clock at the APU clock rate, not
    // once per output sample like this function is currently called
    *out_sample = mix;
}

double fam_apu_get_freq(const FamApu* apu) {
    return NES_CPU_FREQ[apu->region] / 2.0;
}

double fam_apu_get_frame_cycles(const FamApu* apu) {
    return NES_CPU_CYCLES_PER_FRAME[apu->region] / 2.0;
}