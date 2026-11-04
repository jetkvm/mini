#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private heap for allocations which esp_peer routes through its public
 * media_lib allocator ABI. esp_peer also creates the RTP rolling buffer with
 * xRingbufferCreateWithCaps() outside this arena, and Mbed TLS allocates
 * through its own allocator, so this is an ownership boundary rather than a
 * cap on all esp_peer memory. */
#define WEBRTC_PEER_MEMORY_ARENA_OVERHEAD_BYTES (256U * 1024U)

typedef struct {
    bool active;
    uint32_t capacity_bytes;
    uint32_t free_bytes;
    uint32_t minimum_free_bytes;
    uint32_t largest_free_block;
    uint32_t allocated_blocks;
    uint32_t allocation_failures;
    uint32_t begin_failures;
    uint32_t leak_events;
    /* esp_timer time of largest_free_block/allocated_blocks; zero is unknown. */
    uint64_t detailed_sample_timestamp_us;
} webrtc_peer_memory_arena_snapshot_t;

/** Materialize the one-peer private heap before calling esp_peer_open(). */
esp_err_t webrtc_peer_memory_arena_begin(size_t capacity_bytes);

/**
 * Release the private heap after synchronous esp_peer_close().  If any block
 * remains live, keep the backing allocation intact and fail closed so a late
 * callback can never dereference freed PSRAM.
 */
esp_err_t webrtc_peer_memory_arena_end(void);

void webrtc_peer_memory_arena_get_snapshot(
    webrtc_peer_memory_arena_snapshot_t *snapshot);

/** Read current counters/free space without walking the private heap. */
void webrtc_peer_memory_arena_get_snapshot_cached(
    webrtc_peer_memory_arena_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
