#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

size_t util_next_pow2(size_t n) {
    if (n <= 1) {
        return 1;
    }
    
    // If n is already a power of two, don't bump it up
    n--;

#if defined(__GNUC__) || defined(__clang__)
    // Count leading zeros, then shift 1 into the correct position
    #if SIZE_MAX > 0xFFFFFFFF
        int lz = __builtin_clzll((unsigned long long)n);
        return (size_t)1 << (64 - lz);
    #else
        int lz = __builtin_clz((unsigned int)n);
        return (size_t)1 << (32 - lz);
    #endif

#elif defined(_MSC_VER)
    unsigned long index;
    #if SIZE_MAX > 0xFFFFFFFF
        _BitScanReverse64(&index, (unsigned __int64)n);
    #else
        _BitScanReverse(&index, (unsigned long)n);
    #endif
    return (size_t)1 << (index + 1);

#else
    // Portable fallback (bit-smearing approach)
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    #if SIZE_MAX > 0xFFFFFFFF
        n |= n >> 32;
    #endif
    return n + 1;
#endif
}