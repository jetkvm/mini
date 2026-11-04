/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "signaling_budget.h"

int main(void)
{
    _Static_assert(WEBRTC_SIGNAL_QUEUE_DEPTH == 16U,
                   "signaling depth contract changed");
    _Static_assert(WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES == 32U * 1024U,
                   "signaling payload gate changed");
    _Static_assert(WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES == 256U * 1024U,
                   "signaling resident budget changed");

    webrtc_signaling_budget_t budget = {0};
    assert(webrtc_signaling_budget_claim(&budget, 64U * 1024U));
    assert(budget.used == 64U * 1024U);
    assert(budget.high_watermark == budget.used);

    /* Bad releases never wrap and never repair themselves by silently
     * resetting the budget. */
    assert(!webrtc_signaling_budget_release(&budget, 64U * 1024U + 1U));
    assert(budget.used == 64U * 1024U);

    assert(webrtc_signaling_budget_claim(&budget, 192U * 1024U));
    assert(budget.used == WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES);
    assert(!webrtc_signaling_budget_claim(&budget, 1U));
    assert(budget.used == WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES);

    /* A full FIFO can replace its oldest resident charge atomically without
     * a transient over-budget state. */
    assert(webrtc_signaling_budget_try_replace(
        &budget, 64U * 1024U, 32U * 1024U));
    assert(budget.used == 224U * 1024U);
    assert(budget.high_watermark == WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES);

    const size_t before_failed_replace = budget.used;
    assert(!webrtc_signaling_budget_try_replace(
        &budget, 225U * 1024U, 1U));
    assert(budget.used == before_failed_replace);
    assert(!webrtc_signaling_budget_try_replace(
        &budget, 0U, 32U * 1024U + 1U));
    assert(budget.used == before_failed_replace);

    assert(webrtc_signaling_budget_release(&budget, budget.used));
    assert(budget.used == 0U);
    assert(budget.high_watermark == WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES);

    webrtc_signaling_budget_t corrupt = {
        .used = WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES + 1U,
    };
    assert(!webrtc_signaling_budget_claim(&corrupt, 0U));
    assert(corrupt.used == WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES + 1U);
    puts("signaling budget contract: OK");
    return 0;
}
