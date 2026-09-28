#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct FamApu FamApu;
typedef uint8_t (*FamDmcReadFn)(void* user_data, uint16_t addr);

typedef enum {
    FAM_REGISTER_PULSE1_0           = 0x4000,
    FAM_REGISTER_PULSE1_1           = 0x4001,
    FAM_REGISTER_PULSE1_2           = 0x4002,
    FAM_REGISTER_PULSE1_3           = 0x4003,

    FAM_REGISTER_PULSE2_0           = 0x4004,
    FAM_REGISTER_PULSE2_1           = 0x4005,
    FAM_REGISTER_PULSE2_2           = 0x4006,
    FAM_REGISTER_PULSE2_3           = 0x4007,

    FAM_REGISTER_TRIANGLE_0         = 0x4008,
    FAM_REGISTER_TRIANGLE_1         = 0x4009,
    FAM_REGISTER_TRIANGLE_2         = 0x400A,
    FAM_REGISTER_TRIANGLE_3         = 0x400B,

    FAM_REGISTER_NOISE_0            = 0x400C,
    FAM_REGISTER_NOISE_1            = 0x400D,
    FAM_REGISTER_NOISE_2            = 0x400E,
    FAM_REGISTER_NOISE_3            = 0x400F,

    FAM_REGISTER_DMC_0              = 0x4010,
    FAM_REGISTER_DMC_1              = 0x4011,
    FAM_REGISTER_DMC_2              = 0x4012,
    FAM_REGISTER_DMC_3              = 0x4013,

    FAM_REGISTER_STATUS             = 0x4015,
    FAM_REGISTER_FRAME_COUNTER      = 0x4017
} FamRegister;

#ifdef __cplusplus
extern "C" {
#endif

FAM_API size_t FAM_CALL fam_apu_get_memory_required(void);
FAM_API size_t FAM_CALL fam_apu_get_memory_alignment(void);
// NOTE: A resample rate of 0 means no resampling, samples are output at APU clock rate
FAM_API FamResult FAM_CALL fam_apu_init(FamApu** out_apu, void* memory, FamRegion region, uint32_t resample_rate);
FAM_API void FAM_CALL fam_apu_shutdown(FamApu* apu);
FAM_API FamRegion FAM_CALL fam_apu_get_region(const FamApu* apu);
FAM_API FamResult FAM_CALL fam_apu_write_register(FamApu* apu, uint16_t reg, uint8_t data);
FAM_API FamResult FAM_CALL fam_apu_read_register(FamApu* apu, uint16_t reg, uint8_t* out_data);
FAM_API void FAM_CALL fam_apu_set_dmc_reader(FamApu* apu, FamDmcReadFn reader, void* user_data);
// NOTE: Caller is responsible for making sure out_samples has enough room
// If out_samples is NULL, nothing is output
FAM_API int FAM_CALL fam_apu_run(FamApu* apu, int cycles, float* out_samples);
FAM_API double FAM_CALL fam_apu_get_clock_rate(const FamApu* apu);
FAM_API int FAM_CALL fam_apu_get_cycles_for_samples(const FamApu* apu, int sample_count);

#ifdef __cplusplus
}
#endif