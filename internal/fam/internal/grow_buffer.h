#pragma once
#include <fam/common.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// A growable byte buffer.
typedef struct {
    size_t capacity;
    size_t size;
    uint8_t* data;
} GrowBuffer;

static inline FamResult grow_buffer_init(GrowBuffer* buffer, size_t capacity) {
    buffer->data = (uint8_t*)malloc(capacity);
    if (buffer->data == NULL) {
        return FAM_ERROR_OUT_OF_MEMORY;
    }
    buffer->capacity = capacity;
    buffer->size = 0;
    return FAM_SUCCESS;
}

static inline void grow_buffer_free(GrowBuffer* buffer) {
    free(buffer->data);
}

static inline FamResult grow_buffer_write_bytes(GrowBuffer* buffer, uint8_t* bytes, size_t count) {
    // Resize buffer if needed
    if (buffer->capacity < buffer->size + count) {
        uint8_t* new_data = (uint8_t*)realloc(buffer->data, buffer->capacity * 2);
        if (new_data == NULL) {
            return FAM_ERROR_OUT_OF_MEMORY;
        }
        buffer->data = new_data;
        buffer->capacity *= 2;
    }

    uint8_t* ptr = buffer->data + buffer->size;
    memcpy(ptr, bytes, count);
    buffer->size += count;

    return FAM_SUCCESS;
}
