#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct FamApu FamApu;

typedef enum {
    FAM_REGISTER_PULSE1_DUTY         = 0x4000,
    FAM_REGISTER_PULSE1_SWEEP        = 0x4001,
    FAM_REGISTER_PULSE1_TIMER_LO     = 0x4002,
    FAM_REGISTER_PULSE1_TIMER_HI     = 0x4003,

    FAM_REGISTER_PULSE2_DUTY         = 0x4004,
    FAM_REGISTER_PULSE2_SWEEP        = 0x4005,
    FAM_REGISTER_PULSE2_TIMER_LO     = 0x4006,
    FAM_REGISTER_PULSE2_TIMER_HI     = 0x4007,

    FAM_REGISTER_TRIANGLE_COUNTER    = 0x4008,
    FAM_REGISTER_TRIANGLE_UNUSED     = 0x4009,
    FAM_REGISTER_TRIANGLE_TIMER_LO   = 0x400A,
    FAM_REGISTER_TRIANGLE_TIMER_HI   = 0x400B,

    FAM_REGISTER_STATUS              = 0x4015,
    FAM_REGISTER_FRAME_COUNTER       = 0x4017
} FamRegister;

FamApu* fam_apu_init();
void fam_apu_free(FamApu* apu);
FamResult fam_apu_write_register(FamApu* apu, FamRegister reg, uint8_t data);
FamResult fam_apu_read_register(FamApu* apu, FamRegister reg, uint8_t* out_data);
void fam_apu_clock(FamApu* apu, float* out_sample);
double fam_apu_get_freq(FamApu* apu);