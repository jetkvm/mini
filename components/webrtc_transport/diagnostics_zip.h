#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const void *data;
    size_t size;
} diagnostics_zip_entry_t;
typedef bool (*diagnostics_zip_write_fn)(const void *data, size_t size, void *ctx);
/* Stored ZIP entries, streamed without allocating a second copy of the bundle. */
bool diagnostics_zip_write(const diagnostics_zip_entry_t *entries, size_t count,
                           diagnostics_zip_write_fn write, void *ctx);
