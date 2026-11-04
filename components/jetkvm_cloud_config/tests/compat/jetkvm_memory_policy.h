#pragma once

#include <stddef.h>
#include <stdlib.h>

static inline void *jetkvm_memory_policy_control_alloc(size_t size)
{
    return malloc(size);
}

static inline void jetkvm_memory_policy_control_free(void *memory)
{
    free(memory);
}
