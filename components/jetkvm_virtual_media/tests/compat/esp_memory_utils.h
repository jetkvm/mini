/* Host-only DMA address-domain model for upload_dma_contract.h. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

extern uintptr_t test_internal_dma_low;
extern uintptr_t test_internal_dma_high;
extern uintptr_t test_external_dma_low;
extern uintptr_t test_external_dma_high;

static inline bool esp_ptr_dma_capable(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    return address >= test_internal_dma_low &&
           address < test_internal_dma_high;
}

static inline bool esp_ptr_dma_ext_capable(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    return address >= test_external_dma_low &&
           address < test_external_dma_high;
}
