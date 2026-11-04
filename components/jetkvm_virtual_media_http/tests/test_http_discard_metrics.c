/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "http_upload_lifecycle.h"

int main(void)
{
    assert(!http_discard_content_length_valid(0U));
    assert(!http_discard_content_length_valid(
        HTTP_DISCARD_MIN_CONTENT_BYTES - 1U));
    assert(http_discard_content_length_valid(
        HTTP_DISCARD_MIN_CONTENT_BYTES));
    assert(http_discard_content_length_valid(
        HTTP_DISCARD_MAX_CONTENT_BYTES));
    assert(!http_discard_content_length_valid(
        HTTP_DISCARD_MAX_CONTENT_BYTES + 1U));

    assert(!http_discard_content_length_valid(
        (UINT64_C(1) << 32) + HTTP_DISCARD_MIN_CONTENT_BYTES));

    http_discard_receive_metrics_t metrics = http_discard_metrics_init();
    assert(metrics.first_byte_us == -1);
    assert(metrics.bytes == 0U);
    assert(metrics.receive_calls == 0U);

    http_discard_metrics_record(&metrics, 0U, 5000000, true, 5000000);
    assert(metrics.receive_calls == 1U);
    assert(metrics.receive_timeouts == 1U);
    assert(metrics.receive_us == 5000000);
    assert(metrics.max_receive_us == 5000000);
    assert(metrics.first_byte_us == -1);
    assert(metrics.min_receive_bytes == 0U);
    assert(metrics.max_receive_bytes == 0U);

    http_discard_metrics_record(&metrics, 1440U, 100, false, 5000100);
    assert(metrics.receive_calls == 2U);
    assert(metrics.bytes == 1440U);
    assert(metrics.first_byte_us == 5000100);
    assert(metrics.min_receive_bytes == 1440U);
    assert(metrics.max_receive_bytes == 1440U);

    http_discard_metrics_record(&metrics, 65536U, 200, false, 5000300);
    http_discard_metrics_record(&metrics, 512U, 50, false, 5000350);
    assert(metrics.receive_calls == 4U);
    assert(metrics.receive_timeouts == 1U);
    assert(metrics.bytes == 67488U);
    assert(metrics.receive_us == 5000350);
    assert(metrics.max_receive_us == 5000000);
    assert(metrics.first_byte_us == 5000100);
    assert(metrics.min_receive_bytes == 512U);
    assert(metrics.max_receive_bytes == 65536U);

    /* A non-progress socket result is a call but not a timeout or byte. */
    http_discard_metrics_record(&metrics, 0U, -1, false, 5000400);
    assert(metrics.receive_calls == 5U);
    assert(metrics.receive_timeouts == 1U);
    assert(metrics.receive_us == 5000350);
    assert(metrics.bytes == 67488U);

    http_discard_metrics_record(NULL, 1U, 1, false, 1);

    puts("HTTP discard metrics: PASS");
    return 0;
}
