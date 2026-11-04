/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "jetkvm_memory_policy.h"
#endif

/* Host-only unit tests deliberately do not pull the ESP-IDF header graph.
 * Firmware always consumes the global control-arena partition. */
#ifndef JETKVM_MEMORY_CONTROL_SIGNALING_MAX_BYTES
#define JETKVM_MEMORY_CONTROL_SIGNALING_MAX_BYTES (256U * 1024U)
#endif

/* Depth limits object count; the byte budget limits aggregate retained
 * capacity.  A dequeued HTTP event remains charged until its explicit release
 * so a slow response cannot make the producer oversubscribe the control heap. */
#define WEBRTC_SIGNAL_QUEUE_DEPTH 16U
#define WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES (32U * 1024U)
#define WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES \
    JETKVM_MEMORY_CONTROL_SIGNALING_MAX_BYTES

typedef struct {
    size_t used;
    size_t high_watermark;
} webrtc_signaling_budget_t;

/**
 * Atomically replace one existing charge with another.  Callers serialize
 * access.  On failure the budget is unchanged, including when release_bytes
 * would underflow it.
 */
static inline bool webrtc_signaling_budget_try_replace(
    webrtc_signaling_budget_t *budget, size_t release_bytes,
    size_t claim_bytes)
{
    if (budget == NULL || release_bytes > budget->used) {
        return false;
    }
    const size_t remaining = budget->used - release_bytes;
    if (remaining > WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES ||
        claim_bytes > WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES - remaining) {
        return false;
    }
    budget->used = remaining + claim_bytes;
    if (budget->used > budget->high_watermark) {
        budget->high_watermark = budget->used;
    }
    return true;
}

static inline bool webrtc_signaling_budget_claim(
    webrtc_signaling_budget_t *budget, size_t claim_bytes)
{
    return webrtc_signaling_budget_try_replace(budget, 0U, claim_bytes);
}

/** Leave the accounting fail-closed rather than wrapping on a bad release. */
static inline bool webrtc_signaling_budget_release(
    webrtc_signaling_budget_t *budget, size_t release_bytes)
{
    return webrtc_signaling_budget_try_replace(budget, release_bytes, 0U);
}
