#pragma once

#if defined(FAM_SHARED)
    #if defined(_WIN32) || defined(__CYGWIN__)
        #if defined(FAM_BUILD)
            #define FAM_API __declspec(dllexport)
        #else
            #define FAM_API __declspec(dllimport)
        #endif
    #else
        #define FAM_API __attribute__((visibility("default")))
    #endif
#else
    #define FAM_API
#endif

#if defined(_WIN32) && !defined(_WIN64)
    #define FAM_CALL __cdecl
#else
    #define FAM_CALL
#endif

typedef enum {
    FAM_SUCCESS                     = 0,
    FAM_ERROR_UNKNOWN               = -1,
    FAM_ERROR_UNIMPLEMENTED         = -2,
    FAM_ERROR_OUT_OF_MEMORY         = -3,
    FAM_ERROR_INVALID_ARGUMENT      = -4,
    FAM_ERROR_WRITE_ONLY            = -5,
    FAM_ERROR_READ_ONLY             = -6,
    FAM_ERROR_REGION_MISMATCH       = -7,
    FAM_ERROR_INVALID_FORMAT        = -8,
    FAM_ERROR_UNSUPPORTED_VERSION   = -9,
    FAM_ERROR_UNSUPPORTED_FEATURE   = -10,
} FamResult;

typedef enum {
    FAM_REGION_NTSC = 0,
    FAM_REGION_PAL  = 1,
} FamRegion;

typedef enum {
    FAM_AUDIO_F32 = 0,
} FamAudioFormat;
