/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WEBRTC_INBOUND_DROP_STALE = 0,
    WEBRTC_INBOUND_DEFER_CURRENT,
    WEBRTC_INBOUND_DELIVER_CURRENT,
} webrtc_inbound_action_t;

typedef struct {
    bool session_active;
    uint32_t session_id;
    uint32_t generation;
    bool peer_ready;
    bool peer_role_matches;
    uint32_t active_peer_session;
    uint32_t active_peer_generation;
} webrtc_inbound_owner_snapshot_t;

/* Pure ownership policy; the queueing code is responsible for preserving FIFO. */
webrtc_inbound_action_t webrtc_inbound_classify(
    const webrtc_inbound_owner_snapshot_t *snapshot,
    uint32_t event_session_id, uint32_t event_generation);

/*
 * This gate is mutated only while the transport spinlock is held.  Keeping
 * the transition policy independent of FreeRTOS makes close/callback ordering
 * deterministic in host tests.
 */
typedef struct {
    bool active;
    uint32_t session_id;
    uint32_t generation;
} webrtc_cloud_callback_gate_t;

bool webrtc_cloud_callback_gate_try_begin(
    webrtc_cloud_callback_gate_t *gate,
    uint32_t session_id, uint32_t generation);

bool webrtc_cloud_callback_gate_matches(
    const webrtc_cloud_callback_gate_t *gate,
    uint32_t session_id, uint32_t generation);

void webrtc_cloud_callback_gate_end(
    webrtc_cloud_callback_gate_t *gate,
    uint32_t session_id, uint32_t generation);
