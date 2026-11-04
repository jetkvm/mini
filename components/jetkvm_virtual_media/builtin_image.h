/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One stretch of the image that holds non-zero bytes. */
typedef struct {
    uint32_t offset;
    uint32_t length;
} jetkvm_builtin_image_run_t;

/* An image kept in flash as its non-zero runs, in ascending order of offset
 * and without overlap; `data` holds the runs' bytes one after another. Every
 * byte outside the runs is zero. */
typedef struct {
    uint32_t size;
    uint32_t run_count;
    const jetkvm_builtin_image_run_t *runs;
    const uint8_t *data;
} jetkvm_builtin_image_t;

/* The installer disc (builtin_installer_iso.py, generated at build time). */
extern const jetkvm_builtin_image_t jetkvm_builtin_installer_image;

/* Copies `size` bytes of `image` from `offset` into `buffer`. False, and
 * nothing written, for a range that is not inside the image. */
bool jetkvm_builtin_image_read(const jetkvm_builtin_image_t *image,
                               uint64_t offset, void *buffer, size_t size);
