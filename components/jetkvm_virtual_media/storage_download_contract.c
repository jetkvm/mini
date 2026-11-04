/* SPDX-License-Identifier: MIT */

#include "storage_download_contract.h"

#include <stddef.h>

#include "http_range_contract.h"

jetkvm_download_response_t jetkvm_download_check_response(
    int status, int64_t content_length, const char *content_range,
    uint64_t offset, uint64_t *total, uint64_t *skip)
{
    *total = 0U;
    *skip = 0U;
    if (status < 200 || status > 299) {
        return JETKVM_DOWNLOAD_RESPONSE_HTTP_ERROR;
    }
    if (status == 206) {
        uint64_t first = 0U;
        uint64_t last = 0U;
        uint64_t size = 0U;
        if (content_range == NULL ||
            !jetkvm_http_parse_content_range(content_range, &first, &last,
                                             &size)) {
            return JETKVM_DOWNLOAD_RESPONSE_NO_SIZE;
        }
        if (first != offset || last + 1U != size) {
            return JETKVM_DOWNLOAD_RESPONSE_BAD_RANGE;
        }
        *total = size;
        return JETKVM_DOWNLOAD_RESPONSE_OK;
    }
    if (status != 200 || content_length <= 0) {
        return JETKVM_DOWNLOAD_RESPONSE_NO_SIZE;
    }
    if ((uint64_t)content_length < offset) {
        return JETKVM_DOWNLOAD_RESPONSE_BAD_RANGE;
    }
    *total = (uint64_t)content_length;
    *skip = offset;
    return JETKVM_DOWNLOAD_RESPONSE_OK;
}

bool jetkvm_download_progress_due(uint64_t previous_bytes, int64_t previous_us,
                                  uint64_t bytes, int64_t now_us,
                                  uint64_t total, int64_t interval_us)
{
    if (total == 0U || bytes >= total || previous_bytes == 0U) {
        return true;
    }
    if (now_us - previous_us >= interval_us) {
        return true;
    }
    return previous_bytes * 100U / total != bytes * 100U / total;
}
