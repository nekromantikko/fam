#pragma once

typedef enum {
    FAM_SUCCESS                     = 0,
    FAM_ERROR_INVALID_REGISTER      = 1,
    FAM_ERROR_WRITE_ONLY            = 2,
    FAM_ERROR_READ_ONLY             = 3,
} FamResult;