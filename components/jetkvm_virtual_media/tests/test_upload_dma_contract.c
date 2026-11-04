/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "upload_dma_contract.h"

uintptr_t test_internal_dma_low = 0x10000U;
uintptr_t test_internal_dma_high = 0x50000U;
uintptr_t test_external_dma_low = 0x100000U;
uintptr_t test_external_dma_high = 0x180000U;

static bool eligible(uintptr_t address, size_t size, uint64_t offset,
                     bool host_aligned)
{
    return jetkvm_upload_direct_write_eligible(
        (const void *)address, size, offset, 512U, host_aligned);
}

int main(void)
{
    assert(eligible(0x10000U, 512U, 0U, true));
    assert(eligible(0x10000U, 256U * 1024U, 512U, true));
    assert(eligible(0x100000U, 512U, 0U, true));
    assert(eligible(0x100000U, 256U * 1024U, 512U, true));

    assert(!eligible(0x10000U, 512U, 128U, true));
    assert(!eligible(0x10000U, 511U, 0U, true));
    assert(!eligible(0x10000U, 513U, 0U, true));
    assert(!eligible(0x10000U, 512U, 0U, false));
    assert(!eligible(0x80000U, 512U, 0U, true));
    assert(!eligible(0x4ff00U, 512U, 0U, true));
    assert(!jetkvm_upload_direct_write_eligible(
        NULL, 512U, 0U, 512U, true));
    assert(!jetkvm_upload_direct_write_eligible(
        (const void *)0x10000U, 0U, 0U, 512U, true));
    assert(!jetkvm_upload_direct_write_eligible(
        (const void *)(UINTPTR_MAX - 255U), 512U, 0U, 512U, true));

    /* A span starting in internal DMA and ending in external DMA must not be
     * accepted merely because both endpoints are DMA-capable individually. */
    test_internal_dma_low = 0x10000U;
    test_internal_dma_high = 0x10800U;
    test_external_dma_low = 0x10800U;
    test_external_dma_high = 0x11000U;
    assert(!eligible(0x10000U, 4096U, 0U, true));

    test_internal_dma_low = 0x10000U;
    test_internal_dma_high = 0x50000U;
    test_external_dma_low = 0x100000U;
    test_external_dma_high = 0x180000U;
    assert(!jetkvm_upload_owner_staging_required(
        true, (const void *)0x10000U, 64U * 1024U));
    assert(jetkvm_upload_owner_staging_required(
        true, (const void *)0x100000U, 64U * 1024U));
    assert(jetkvm_upload_owner_staging_required(
        true, (const void *)0x80000U, 64U * 1024U));
    assert(!jetkvm_upload_owner_staging_required(
        false, (const void *)0x100000U, 64U * 1024U));
    assert(jetkvm_upload_owner_staging_required(true, NULL, 512U));
    assert(jetkvm_upload_owner_staging_required(
        true, (const void *)(UINTPTR_MAX - 255U), 512U));

    assert(jetkvm_upload_bounce_slice_size(
               256U * 1024U, 64U * 1024U, 0U, 512U) == 64U * 1024U);
    assert(jetkvm_upload_bounce_slice_size(
               256U * 1024U, 64U * 1024U, 128U, 512U) == 384U);
    assert(jetkvm_upload_bounce_slice_size(
               256U, 64U * 1024U, 128U, 512U) == 256U);
    assert(jetkvm_upload_bounce_slice_size(123U, 0U, 0U, 512U) == 0U);

    assert(jetkvm_upload_pipeline_half_capacity(
               64U * 1024U, 512U, 128U) == 32U * 1024U);
    assert(jetkvm_upload_pipeline_half_capacity(
               64U * 1024U - 128U, 512U, 128U) == 0U);
    assert(jetkvm_upload_pipeline_half_capacity(
               64U * 1024U, 0U, 128U) == 0U);
    assert(jetkvm_upload_pipeline_half_capacity(
               64U * 1024U, 512U, 0U) == 0U);

    /* A resumed stream first closes its partial sector, then alternates exact
     * 32 KiB windows and preserves a final non-sector tail. */
    const size_t half = 32U * 1024U;
    size_t remaining = 3U * half + 512U - 128U + 123U;
    uint64_t offset = 128U;
    unsigned half_index = 0U;
    size_t slices[5] = {0};
    size_t slice_count = 0U;
    while (remaining > 0U) {
        size_t slice = jetkvm_upload_bounce_slice_size(
            remaining, half, offset, 512U);
        assert(slice > 0U && slice <= half);
        assert(half_index < 2U);
        slices[slice_count++] = slice;
        remaining -= slice;
        offset += slice;
        half_index ^= 1U;
    }
    assert(slice_count == 5U);
    assert(slices[0] == 384U);
    assert(slices[1] == half);
    assert(slices[2] == half);
    assert(slices[3] == half);
    assert(slices[4] == 123U);
    assert(offset == 512U + 3U * half + 123U);

    puts("virtual-media upload DMA contract: PASS");
    return 0;
}
