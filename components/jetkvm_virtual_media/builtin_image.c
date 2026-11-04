/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "builtin_image.h"

#include <string.h>

bool jetkvm_builtin_image_read(const jetkvm_builtin_image_t *image,
                               uint64_t offset, void *buffer, size_t size)
{
    if (image == NULL || (buffer == NULL && size > 0U) ||
        offset > image->size || size > image->size - offset) {
        return false;
    }
    uint8_t *out = buffer;
    if (size > 0U) {
        memset(out, 0, size);
    }
    const uint64_t end = offset + size;
    const uint8_t *data = image->data;
    for (uint32_t i = 0U; i < image->run_count && size > 0U; ++i) {
        const uint64_t run_start = image->runs[i].offset;
        const uint64_t run_end = run_start + image->runs[i].length;
        if (run_start >= end) {
            break;
        }
        if (run_end > offset) {
            const uint64_t from = run_start > offset ? run_start : offset;
            const uint64_t to = run_end < end ? run_end : end;
            memcpy(out + (from - offset), data + (from - run_start),
                   (size_t)(to - from));
        }
        data += image->runs[i].length;
    }
    return true;
}
