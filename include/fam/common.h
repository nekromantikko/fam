#pragma once
#include <stddef.h>

// Every public function is marked FAM_API. It's empty for a static build (the default) and only
// does anything when fam is built as a shared library, which is defined by:
// - FAM_SHARED: fam is a shared library. Has to be defined when building AND when using it
// - FAM_BUILD: we're compiling fam itself, rather than using it
#if defined(FAM_SHARED)
    #if defined(_WIN32)
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

// TODO: A FAMCALL macro (__cdecl on Windows, empty elsewhere) next to FAM_API, if fam is ever
// built for 32 bit x86. C defaults to cdecl while C# P/Invoke defaults to stdcall, and getting
// that wrong corrupts the stack. Only 32 bit x86 has more than one calling convention, so x64
// and ARM builds don't care, and bindings can also just ask for cdecl themselves (SDL solves
// this with its SDLCALL macro).

typedef enum {
    FAM_SUCCESS                     = 0,
    FAM_ERROR_UNKNOWN               = -1,
    FAM_ERROR_UNIMPLEMENTED         = -2,
    FAM_ERROR_OUT_OF_MEMORY         = -3,
    FAM_ERROR_INVALID_ARGUMENT      = -4,
    FAM_ERROR_WRITE_ONLY            = -5,
    FAM_ERROR_READ_ONLY             = -6,
    FAM_ERROR_MACHINE_MISMATCH      = -7,
    FAM_ERROR_INVALID_FORMAT        = -8,
    FAM_ERROR_UNSUPPORTED_VERSION   = -9,
    FAM_ERROR_UNSUPPORTED_FEATURE   = -10,
} FamResult;

typedef enum {
    FAM_MACHINE_NTSC = 0,
    FAM_MACHINE_PAL  = 1,
} FamMachine;

typedef enum {
    FAM_AUDIO_F32 = 0,
} FamAudioFormat;