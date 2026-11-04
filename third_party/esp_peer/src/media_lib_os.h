/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  This header provide weak realization of MOCK media_lib API
 *         So that it can run without media_lib
 */
#define MEDIA_LIB_MAX_LOCK_TIME 0xFFFFFFFF

typedef void* media_lib_mutex_handle_t;

void* media_lib_malloc(size_t size);

void* media_lib_calloc(size_t nmemb, size_t size);

void* media_lib_realloc(void *ptr, size_t size);

void media_lib_free(void *ptr);

/* Keep copied protocol strings in the same allocation domain as their owner. */
static inline char *media_lib_strdup(const char *value)
{
    size_t size = strlen(value) + 1;
    char *copy = media_lib_malloc(size);
    if (copy != NULL) {
        memcpy(copy, value, size);
    }
    return copy;
}

int media_lib_mutex_create(media_lib_mutex_handle_t *mutex);

int media_lib_mutex_destroy(media_lib_mutex_handle_t mutex);

int media_lib_mutex_lock(media_lib_mutex_handle_t mutex, uint32_t timeout);

int media_lib_mutex_unlock(media_lib_mutex_handle_t mutex);

void media_lib_thread_sleep(int ms);

#ifdef __cplusplus
}
#endif