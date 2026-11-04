/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* This is deliberately much longer than the supported 150 ms WAN RTT while
 * still bounding the lifetime of the writer token, internal-DMA staging,
 * PSRAM ingress buffers and HTTP handler held by an abandoned TCP request. */
#define HTTP_UPLOAD_RECEIVE_IDLE_TIMEOUT_US INT64_C(30000000)

/* The discard endpoint is a diagnostic build feature, not a general-purpose
 * upload surface. Keep each run long enough to escape startup noise while
 * bounding how long one local request can occupy the
 * synchronous HTTP server task. */
#define HTTP_DISCARD_BUFFER_SIZE (256U * 1024U)
#define HTTP_DISCARD_MIN_CONTENT_BYTES (16U * 1024U * 1024U)
#define HTTP_DISCARD_MAX_CONTENT_BYTES (256U * 1024U * 1024U)

typedef struct {
    uint64_t bytes;
    uint32_t receive_calls;
    uint32_t receive_timeouts;
    uint32_t min_receive_bytes;
    uint32_t max_receive_bytes;
    int64_t receive_us;
    int64_t max_receive_us;
    int64_t first_byte_us;
} http_discard_receive_metrics_t;

static inline bool http_upload_receive_idle_timed_out(
    int64_t now_us, int64_t last_receive_progress_us)
{
    return last_receive_progress_us > 0 && now_us >= last_receive_progress_us &&
           now_us - last_receive_progress_us >=
               HTTP_UPLOAD_RECEIVE_IDLE_TIMEOUT_US;
}

static inline size_t http_upload_resume_prefix(uint64_t uploaded_bytes,
                                               size_t block_size)
{
    if (block_size == 0U) {
        return 0U;
    }
    const size_t remainder = (size_t)(uploaded_bytes % block_size);
    return remainder == 0U ? 0U : block_size - remainder;
}

static inline bool http_discard_content_length_valid(uint64_t content_length)
{
    return content_length >= HTTP_DISCARD_MIN_CONTENT_BYTES &&
           content_length <= HTTP_DISCARD_MAX_CONTENT_BYTES;
}

static inline http_discard_receive_metrics_t http_discard_metrics_init(void)
{
    return (http_discard_receive_metrics_t) {
        .first_byte_us = -1,
    };
}

static inline void http_discard_metrics_record(
    http_discard_receive_metrics_t *metrics, size_t received_bytes,
    int64_t receive_us, bool timed_out, int64_t progress_us)
{
    if (metrics == NULL) {
        return;
    }
    metrics->receive_calls++;
    if (receive_us > 0) {
        metrics->receive_us += receive_us;
        if (receive_us > metrics->max_receive_us) {
            metrics->max_receive_us = receive_us;
        }
    }
    if (timed_out) {
        metrics->receive_timeouts++;
        return;
    }
    if (received_bytes == 0U) {
        return;
    }
    const uint32_t received = (uint32_t)received_bytes;
    metrics->bytes += received_bytes;
    if (metrics->min_receive_bytes == 0U ||
        received < metrics->min_receive_bytes) {
        metrics->min_receive_bytes = received;
    }
    if (received > metrics->max_receive_bytes) {
        metrics->max_receive_bytes = received;
    }
    if (metrics->first_byte_us < 0 && progress_us >= 0) {
        metrics->first_byte_us = progress_us;
    }
}
