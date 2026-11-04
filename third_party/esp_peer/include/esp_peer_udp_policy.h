/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#pragma once

#include <stdbool.h>
#include <stdatomic.h>

#include "esp_peer_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Source-visible UDP policy extension used by strong application hooks. */
typedef struct {
    int             fd;
    int             ipv6_fd;
    uint32_t        sin6_scope_id;
    esp_peer_addr_t bind_addr;
    long long int   timeout_sec;
    long int        timeout_usec;
    atomic_int      user_count;
} udp_socket_t;

void udp_blocking_timeout(udp_socket_t *udp_socket, long long int ms);
bool udp_socket_recvfrom_poll_allowed(bool *nowait);
void udp_socket_recvfrom_poll_complete(int result);

#ifdef __cplusplus
}
#endif
