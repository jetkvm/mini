#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t internal_bytes;
    size_t internal_largest_block;
    size_t dma_bytes;
    size_t dma_largest_block;
    size_t psram_bytes;
    size_t psram_largest_block;
} jetkvm_memory_policy_request_t;

typedef struct {
    uint32_t generation;
    uint8_t slot;
    bool active;
} jetkvm_memory_policy_reservation_t;

bool jetkvm_memory_policy_wait_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation);
void jetkvm_memory_policy_release(
    jetkvm_memory_policy_reservation_t *reservation);
