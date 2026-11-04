/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque registration token. Zero is always invalid. */
typedef uint32_t dma_cache_fastpath_handle_t;

#define DMA_CACHE_FASTPATH_INVALID_HANDLE ((dma_cache_fastpath_handle_t)0)

typedef enum {
    DMA_CACHE_FASTPATH_STATE_INVALID = 0,
    /** The whole region is cache-invalid and only DMA engines may access it. */
    DMA_CACHE_FASTPATH_STATE_DEVICE_EXCLUSIVE,
    /** Fast-path skipping is suspended; all cache operations pass to IDF. */
    DMA_CACHE_FASTPATH_STATE_CPU_OR_FALLBACK,
} dma_cache_fastpath_state_t;

typedef struct {
    void *address;
    size_t size;
    dma_cache_fastpath_state_t state;
    uint64_t skipped_c2m_calls;
    uint64_t skipped_c2m_bytes;
    uint64_t skipped_m2c_calls;
    uint64_t skipped_m2c_bytes;
    uint64_t passthrough_calls;
    uint64_t unexpected_overlap_calls;
} dma_cache_fastpath_stats_t;

typedef struct {
    uint64_t skipped_c2m_calls;
    uint64_t skipped_c2m_bytes;
    uint64_t skipped_m2c_calls;
    uint64_t skipped_m2c_bytes;
    uint64_t passthrough_calls;
    uint64_t unexpected_overlap_calls;
} dma_cache_fastpath_global_stats_t;

/**
 * Register one whole DMA-owned cacheable buffer.
 *
 * The call performs one real C2M writeback+invalidate before enabling the
 * fast path.  Afterwards, the wrapper skips only data-cache msync calls whose
 * address and size exactly equal the registered region.  This matches the
 * full-frame calls made by CSI, PPA SRM, and the H.264 input path without
 * affecting descriptors, H.264 slice headers, or H.264 output buffers.
 *
 * Contract: no CPU load/store may touch the region while it is in
 * DEVICE_EXCLUSIVE state, and registration must not race an in-flight DMA.
 * Register exact active-frame sizes, not a larger allocation capacity.
 */
esp_err_t dma_cache_fastpath_register(void *address, size_t size,
                                      dma_cache_fastpath_handle_t *out_handle);

/**
 * Hand a registered region to the CPU/fallback path.
 *
 * A real M2C invalidate is performed.  Subsequent msync calls pass through to
 * IDF until dma_cache_fastpath_device_rearm() succeeds.
 */
esp_err_t dma_cache_fastpath_cpu_acquire(dma_cache_fastpath_handle_t handle);

/**
 * Return a CPU/fallback region to exclusive DMA ownership.
 *
 * A real full-region C2M writeback+invalidate is performed before skipping is
 * re-enabled, so prior CPU writes cannot be lost or remain cache-resident.
 */
esp_err_t dma_cache_fastpath_device_rearm(dma_cache_fastpath_handle_t handle);

/**
 * Remove a registration while no DMA is in flight.
 *
 * The buffer is first acquired for the CPU with a real M2C invalidate.
 */
esp_err_t dma_cache_fastpath_unregister(dma_cache_fastpath_handle_t handle);

/** Copy one active registration's counters and state. */
esp_err_t dma_cache_fastpath_get_stats(dma_cache_fastpath_handle_t handle,
                                       dma_cache_fastpath_stats_t *out_stats);

/** Copy process-lifetime aggregate counters, including removed regions. */
void dma_cache_fastpath_get_global_stats(dma_cache_fastpath_global_stats_t *out_stats);

/**
 * Runtime kill switch.
 *
 * Disabling makes every wrapped call pass through and suspends all registered
 * regions. Re-enabling the switch does not silently restore skipping; callers
 * must explicitly device_rearm() each desired region.
 */
void dma_cache_fastpath_set_enabled(bool enabled);

bool dma_cache_fastpath_is_enabled(void);

#ifdef __cplusplus
}
#endif
