#pragma once
#include <stdint.h>

// Hardware tables needed outside apu.c. The player uses NOISE_PERIOD to pitch-shift noise
// sound effects by choosing a different period index.

// NOTE: These noise period values are half what is written on Nesdev, 
// because they're APU cycles instead of CPU cycles
static const uint16_t NOISE_PERIOD[2][16] = {
    { 2, 4, 8, 16, 32, 48, 64, 80, 101, 127, 190, 254, 381, 508, 1017, 2034 }, // NTSC
    { 2, 4, 7, 15, 30, 44, 59, 74, 94, 118, 177, 236, 354, 472, 945, 1889 }    // PAL
};
