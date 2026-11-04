/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    JETKVM_DOWNLOAD_RESPONSE_OK = 0,
    /* Not a 2xx answer (after redirects). */
    JETKVM_DOWNLOAD_RESPONSE_HTTP_ERROR,
    /* 2xx without a usable object size. */
    JETKVM_DOWNLOAD_RESPONSE_NO_SIZE,
    /* A 206 whose Content-Range does not continue at the requested offset. */
    JETKVM_DOWNLOAD_RESPONSE_BAD_RANGE,
} jetkvm_download_response_t;

/**
 * Check the headers of a GET that asked for the object from `offset` on
 * (`Range: bytes=<offset>-` when offset > 0). On success, `total` is the
 * whole object's size and `skip` the leading body bytes to discard: a server
 * that ignores Range answers 200 with the whole object.
 */
jetkvm_download_response_t jetkvm_download_check_response(
    int status, int64_t content_length, const char *content_range,
    uint64_t offset, uint64_t *total, uint64_t *skip);

/**
 * True when a progress update is due: the first and last byte, each new
 * whole percent, or at least `interval_us` after the previous update.
 */
bool jetkvm_download_progress_due(uint64_t previous_bytes, int64_t previous_us,
                                  uint64_t bytes, int64_t now_us,
                                  uint64_t total, int64_t interval_us);
