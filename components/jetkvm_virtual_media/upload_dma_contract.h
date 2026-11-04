/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_memory_utils.h"

/* FatFs passes complete sectors straight to disk_write().  The SDMMC host's
 * alignment callback deliberately checks cache-line alignment only, so keep
 * the memory-domain test here: an aligned flash/invalid pointer must never be
 * mistaken for DMA-capable RAM.  Checking both ends also rejects a span that
 * crosses from one DMA domain into another. */
static inline bool jetkvm_upload_direct_write_eligible(
    const void *data, size_t size, uint64_t file_offset, size_t sector_size,
    bool host_aligned)
{
    if (data == NULL || size == 0U || sector_size == 0U || !host_aligned ||
        file_offset % sector_size != 0U || size % sector_size != 0U) {
        return false;
    }

    uintptr_t first = (uintptr_t)data;
    if (size - 1U > UINTPTR_MAX - first) {
        return false;
    }
    const void *last = (const void *)(first + size - 1U);
    bool internal = esp_ptr_dma_capable(data) && esp_ptr_dma_capable(last);
    bool external =
        esp_ptr_dma_ext_capable(data) && esp_ptr_dma_ext_capable(last);
    return internal || external;
}

/* A transport which retains the conservative staging policy has paid for an
 * internal-DMA buffer. Keep genuinely internal DMA sources eligible for direct
 * I/O, but route PSRAM and non-DMA sources through that buffer. HTTP on P4 can
 * opt out of this policy and is still guarded by direct_write_eligible(). */
static inline bool jetkvm_upload_owner_staging_required(
    bool owner_staging, const void *data, size_t size)
{
    if (!owner_staging) {
        return false;
    }
    if (data == NULL || size == 0U) {
        return true;
    }
    uintptr_t first = (uintptr_t)data;
    if (size - 1U > UINTPTR_MAX - first) {
        return true;
    }
    const void *last = (const void *)(first + size - 1U);
    return !(esp_ptr_dma_capable(data) && esp_ptr_dma_capable(last));
}

/* A resumed .incomplete file can end mid-sector. Use one short prefix to
 * regain sector alignment, then consume the full staging window. */
static inline size_t jetkvm_upload_bounce_slice_size(
    size_t remaining, size_t capacity, uint64_t file_offset,
    size_t sector_size)
{
    size_t slice = remaining < capacity ? remaining : capacity;
    if (slice == 0U || sector_size == 0U) {
        return slice;
    }
    size_t misalignment = (size_t)(file_offset % sector_size);
    if (misalignment != 0U) {
        size_t prefix = sector_size - misalignment;
        if (prefix < slice) {
            slice = prefix;
        }
    }
    return slice;
}

/* Split one staging allocation into two interchangeable DMA windows.  Refuse
 * a split unless both bases retain the storage and host alignment contracts;
 * callers then alternate the halves without allocating a second buffer. */
static inline size_t jetkvm_upload_pipeline_half_capacity(
    size_t staging_capacity, size_t sector_size, size_t host_alignment)
{
    if (staging_capacity == 0U || sector_size == 0U ||
        host_alignment == 0U || staging_capacity % 2U != 0U) {
        return 0U;
    }
    size_t half = staging_capacity / 2U;
    return half % sector_size == 0U && half % host_alignment == 0U ?
               half : 0U;
}
