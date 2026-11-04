/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_TIME_SYNC_MAX_USER_SERVERS 3U
#define JETKVM_TIME_SYNC_NAME_CAPACITY 64U
#define JETKVM_TIME_SYNC_FALLBACK_SERVER "time.apple.com"

/** Query order: user servers first, then the public fallback when allowed. */
typedef struct {
    const char *servers[JETKVM_TIME_SYNC_MAX_USER_SERVERS];
    size_t count;
    bool fallback;
} jetkvm_time_sync_config_t;

/**
 * Copy the SNTP query order for @p config into @p names. Blank and duplicate
 * entries are dropped. Returns the number of names written, at most @p capacity.
 */
size_t jetkvm_time_sync_server_list(const jetkvm_time_sync_config_t *config,
                                    char names[][JETKVM_TIME_SYNC_NAME_CAPACITY],
                                    size_t capacity);

#ifdef __cplusplus
}
#endif
