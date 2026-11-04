/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "controlled_session_policy.h"

#include <stddef.h>

webrtc_inbound_action_t webrtc_inbound_classify(
    const webrtc_inbound_owner_snapshot_t *snapshot,
    uint32_t event_session_id, uint32_t event_generation)
{
    if (snapshot == NULL || !snapshot->session_active ||
        event_session_id != snapshot->session_id ||
        event_generation != snapshot->generation) {
        return WEBRTC_INBOUND_DROP_STALE;
    }
    if (!snapshot->peer_ready || !snapshot->peer_role_matches ||
        event_session_id != snapshot->active_peer_session ||
        event_generation != snapshot->active_peer_generation) {
        return WEBRTC_INBOUND_DEFER_CURRENT;
    }
    return WEBRTC_INBOUND_DELIVER_CURRENT;
}

bool webrtc_cloud_callback_gate_try_begin(
    webrtc_cloud_callback_gate_t *gate,
    uint32_t session_id, uint32_t generation)
{
    if (gate == NULL || gate->active || session_id == 0 || generation == 0) {
        return false;
    }
    gate->active = true;
    gate->session_id = session_id;
    gate->generation = generation;
    return true;
}

bool webrtc_cloud_callback_gate_matches(
    const webrtc_cloud_callback_gate_t *gate,
    uint32_t session_id, uint32_t generation)
{
    return gate != NULL && gate->active && gate->session_id == session_id &&
           gate->generation == generation;
}

void webrtc_cloud_callback_gate_end(
    webrtc_cloud_callback_gate_t *gate,
    uint32_t session_id, uint32_t generation)
{
    if (!webrtc_cloud_callback_gate_matches(gate, session_id, generation)) {
        return;
    }
    gate->active = false;
    gate->session_id = 0;
    gate->generation = 0;
}
