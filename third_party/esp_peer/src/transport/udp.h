/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#pragma once

#include <stdbool.h>
#include "esp_peer_types.h"
#include "esp_peer_udp_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

int udp_socket_open(udp_socket_t *udp_socket, bool ipv6_support);

int udp_socket_bind(udp_socket_t *udp_socket, esp_peer_addr_t *addr);

void udp_socket_close(udp_socket_t *udp_socket);

int udp_socket_sendto(udp_socket_t *udp_socket, esp_peer_addr_t *addr, const uint8_t *buf, int len);

int udp_socket_recvfrom_nowait(udp_socket_t *udp_socket, esp_peer_addr_t *addr, uint8_t *buf, int len, bool nowait);

/* Optional policy seam around one receive attempt. The default implementation
 * always permits the poll and preserves the caller's blocking mode. */
bool udp_socket_recvfrom_poll_allowed(bool *nowait);

void udp_socket_recvfrom_poll_complete(int result);

int udp_get_local_address(udp_socket_t *udp_socket, bool ipv6, esp_peer_addr_t *addr);

int udp_socket_get_host_address(udp_socket_t *udp_socket, esp_peer_addr_t *addr);

int udp_resolve_mdns_host(const char *host, esp_peer_addr_t *addr);

void udp_blocking_timeout(udp_socket_t *udp_socket, long long int ms);

#ifdef __cplusplus
}
#endif
