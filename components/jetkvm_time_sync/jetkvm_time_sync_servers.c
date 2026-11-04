/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */
#include "jetkvm_time_sync_servers.h"

#include <string.h>

static bool listed(const char *name, char names[][JETKVM_TIME_SYNC_NAME_CAPACITY], size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(names[i], name) == 0) return true;
    }
    return false;
}

static size_t add(const char *name, char names[][JETKVM_TIME_SYNC_NAME_CAPACITY],
                  size_t count, size_t capacity)
{
    if (!name || !name[0] || strlen(name) >= JETKVM_TIME_SYNC_NAME_CAPACITY ||
        count >= capacity || listed(name, names, count)) return count;
    strcpy(names[count], name);
    return count + 1;
}

size_t jetkvm_time_sync_server_list(const jetkvm_time_sync_config_t *config,
                                    char names[][JETKVM_TIME_SYNC_NAME_CAPACITY],
                                    size_t capacity)
{
    size_t count = 0;
    if (!config) return add(JETKVM_TIME_SYNC_FALLBACK_SERVER, names, 0, capacity);
    size_t user = config->count < JETKVM_TIME_SYNC_MAX_USER_SERVERS ?
        config->count : JETKVM_TIME_SYNC_MAX_USER_SERVERS;
    for (size_t i = 0; i < user; ++i) count = add(config->servers[i], names, count, capacity);
    if (config->fallback) count = add(JETKVM_TIME_SYNC_FALLBACK_SERVER, names, count, capacity);
    return count;
}
