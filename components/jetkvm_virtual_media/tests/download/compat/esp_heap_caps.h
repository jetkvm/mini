#pragma once

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM (1U << 10)
#define MALLOC_CAP_8BIT (1U << 2)

static inline void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps)
{
    (void)caps;
    return aligned_alloc(alignment, size);
}

static inline void heap_caps_free(void *memory)
{
    free(memory);
}
