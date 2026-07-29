#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

// A bounds-checked cursor over an in-memory buffer. Once a read/skip/seek
// runs past the end, the error flag latches and all subsequent operations
// become no-ops, so callers can parse optimistically and check error once.
typedef struct {
    uint8_t* const start;
    uint8_t* pos;
    uint8_t* const end;
    bool error;
} BufferReader;

static inline BufferReader buffer_reader_init(const void* buffer, size_t size) {
    BufferReader reader = {
        .start = (uint8_t*)buffer,
        .pos = (uint8_t*)buffer,
        .end = (uint8_t*)buffer + size,
        .error = false
    };
    return reader;
}

static inline void buffer_reader_read(BufferReader* reader, void* dst, size_t count) {
    if (reader->error) {
        return;
    }

    // Trying to read past block end
    if (count > (size_t)(reader->end - reader->pos)) {
        reader->pos = reader->end;
        reader->error = true;
        return;
    }

    memcpy(dst, (void*)reader->pos, count);
    reader->pos += count;
}

static inline void buffer_reader_skip(BufferReader* reader, size_t count) {
    if (reader->error) {
        return;
    }

    // Trying to skip past block end
    if (count > (size_t)(reader->end - reader->pos)) {
        reader->pos = reader->end;
        reader->error = true;
        return;
    }

    reader->pos += count;
}

static inline void buffer_reader_seek(BufferReader* reader, size_t offset) {
    if (reader->error) {
        return;
    }

    if (offset > (size_t)(reader->end - reader->start)) {
        reader->pos = reader->end;
        reader->error = true;
        return;
    }

    reader->pos = reader->start + offset;
}

static inline size_t buffer_reader_size(const BufferReader* reader) {
    return (size_t)(reader->end - reader->start);
}

static inline size_t buffer_reader_remaining(const BufferReader* reader) {
    return (size_t)(reader->end - reader->pos);
}

static inline size_t buffer_reader_tell(const BufferReader* reader) {
    return (size_t)(reader->pos - reader->start);
}