/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "srtp_fail_closed.h"

#define ESP_PEER_FAIL_CLOSED_ERROR (-1)

/* Egress handles stay opaque; these guards only inspect packet lengths. */
typedef void *esp_peer_agent_handle_t;
typedef void *esp_peer_rolling_buffer_handle_t;

int __real_agent_send(esp_peer_agent_handle_t agent, const uint8_t *packet, int bytes);
int __real_agent_send_high_priority(esp_peer_agent_handle_t agent, const uint8_t *packet, int bytes);
int __real_agent_send_to_port(esp_peer_agent_handle_t agent, int port, const uint8_t *packet, int bytes);
int __real_rtp_rolling_buffer_insert(esp_peer_rolling_buffer_handle_t rolling, uint32_t uuid,
                                     uint8_t *packet, size_t bytes, uint8_t *key);

int esp_peer_srtp_output_bytes(int status, size_t output_bytes)
{
    if (status != 0 || output_bytes > INT_MAX) {
        return 0;
    }
    return (int)output_bytes;
}

int __wrap_agent_send(esp_peer_agent_handle_t agent, const uint8_t *packet, int bytes)
{
    if (bytes <= 0) {
        return ESP_PEER_FAIL_CLOSED_ERROR;
    }
    return __real_agent_send(agent, packet, bytes);
}

int __wrap_agent_send_high_priority(esp_peer_agent_handle_t agent, const uint8_t *packet, int bytes)
{
    if (bytes <= 0) {
        return ESP_PEER_FAIL_CLOSED_ERROR;
    }
    return __real_agent_send_high_priority(agent, packet, bytes);
}

int __wrap_agent_send_to_port(esp_peer_agent_handle_t agent, int port,
                              const uint8_t *packet, int bytes)
{
    if (bytes <= 0) {
        return ESP_PEER_FAIL_CLOSED_ERROR;
    }
    return __real_agent_send_to_port(agent, port, packet, bytes);
}

int __wrap_rtp_rolling_buffer_insert(esp_peer_rolling_buffer_handle_t rolling, uint32_t uuid,
                                     uint8_t *packet, size_t bytes, uint8_t *key)
{
    if (bytes == 0) {
        return ESP_PEER_FAIL_CLOSED_ERROR;
    }
    return __real_rtp_rolling_buffer_insert(rolling, uuid, packet, bytes, key);
}
