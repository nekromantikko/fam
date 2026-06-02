#pragma once

typedef enum {
    FAM_SUCCESS                     = 0,
    FAM_ERROR_UNKNOWN               = -1,
    FAM_ERROR_UNIMPLEMENTED         = -2,
    FAM_ERROR_OUT_OF_MEMORY         = -3,
    FAM_ERROR_INVALID_ARGUMENT      = -4,
    FAM_ERROR_WRITE_ONLY            = -5,
    FAM_ERROR_READ_ONLY             = -6,
    FAM_ERROR_IO                    = -7,
    FAM_ERROR_INVALID_FORMAT        = -8,
    FAM_ERROR_UNSUPPORTED_VERSION   = -9,
    FAM_ERROR_UNSUPPORTED_FEATURE   = -10,
} FamResult;