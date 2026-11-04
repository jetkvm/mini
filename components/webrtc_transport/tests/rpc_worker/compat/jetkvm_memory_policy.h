#pragma once
#include <stddef.h>

#define JETKVM_MEMORY_RPC_SCRATCH_BYTES (128U * 1024U)

/* The control arena, in control_arena.c: malloc, counting the blocks still
 * held, and failing every allocation of control_arena_fail_size bytes. */
extern size_t control_arena_blocks;
extern size_t control_arena_fail_size;
void *jetkvm_memory_policy_control_alloc(size_t size);
void jetkvm_memory_policy_control_free(void *memory);
