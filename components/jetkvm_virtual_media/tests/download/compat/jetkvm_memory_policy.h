#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t bytes;
} jetkvm_memory_policy_reservation_t;

bool jetkvm_memory_policy_wait_reserve_download(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation);
bool jetkvm_memory_policy_wait_reserve_psram(
    const char *owner, size_t bytes, jetkvm_memory_policy_reservation_t *reservation);
void jetkvm_memory_policy_release(jetkvm_memory_policy_reservation_t *reservation);
void *jetkvm_memory_policy_control_alloc(size_t size);
void jetkvm_memory_policy_control_free(void *memory);
