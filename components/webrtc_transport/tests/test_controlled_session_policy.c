/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>

#include "controlled_session_policy.h"

static void test_immediate_offer_and_candidate_are_deferred(void)
{
    const uint32_t session_id = 11;
    const uint32_t generation = 41;
    webrtc_inbound_owner_snapshot_t created = {
        .session_active = true,
        .session_id = session_id,
        .generation = generation,
        .peer_ready = false,
    };

    /* The offer is queue head and the candidate is directly behind it. */
    assert(webrtc_inbound_classify(
               &created, session_id, generation) ==
           WEBRTC_INBOUND_DEFER_CURRENT);
    assert(webrtc_inbound_classify(
               &created, session_id, generation) ==
           WEBRTC_INBOUND_DEFER_CURRENT);

    created.peer_ready = true;
    created.peer_role_matches = true;
    created.active_peer_session = session_id;
    created.active_peer_generation = generation;
    assert(webrtc_inbound_classify(
               &created, session_id, generation) ==
           WEBRTC_INBOUND_DELIVER_CURRENT);
    assert(webrtc_inbound_classify(
               &created, session_id, generation) ==
           WEBRTC_INBOUND_DELIVER_CURRENT);
}

static void test_rebuild_drops_only_stale_generation(void)
{
    const uint32_t session_id = 22;
    webrtc_inbound_owner_snapshot_t rebuilding = {
        .session_active = true,
        .session_id = session_id,
        .generation = 52,
        .peer_ready = false,
        .peer_role_matches = true,
        .active_peer_session = session_id,
        .active_peer_generation = 51,
    };

    assert(webrtc_inbound_classify(&rebuilding, session_id, 51) ==
           WEBRTC_INBOUND_DROP_STALE);
    assert(webrtc_inbound_classify(&rebuilding, session_id - 1U, 52) ==
           WEBRTC_INBOUND_DROP_STALE);
    assert(webrtc_inbound_classify(&rebuilding, session_id, 52) ==
           WEBRTC_INBOUND_DEFER_CURRENT);

    rebuilding.peer_ready = true;
    rebuilding.active_peer_generation = 52;
    assert(webrtc_inbound_classify(&rebuilding, session_id, 52) ==
           WEBRTC_INBOUND_DELIVER_CURRENT);

    rebuilding.session_active = false;
    assert(webrtc_inbound_classify(&rebuilding, session_id, 52) ==
           WEBRTC_INBOUND_DROP_STALE);
}

static void test_close_waits_for_serial_callback_gate(void)
{
    webrtc_cloud_callback_gate_t gate = { 0 };
    const uint32_t session_id = 33;
    const uint32_t generation = 61;

    /* A signal callback is in flight when close starts. */
    assert(webrtc_cloud_callback_gate_try_begin(
        &gate, session_id, generation));
    assert(webrtc_cloud_callback_gate_matches(
        &gate, session_id, generation));
    assert(!webrtc_cloud_callback_gate_try_begin(
        &gate, session_id, generation));

    /* A stale completion cannot release the current callback's ctx. */
    webrtc_cloud_callback_gate_end(&gate, session_id, generation - 1U);
    assert(webrtc_cloud_callback_gate_matches(
        &gate, session_id, generation));
    webrtc_cloud_callback_gate_end(&gate, session_id, generation);
    assert(!webrtc_cloud_callback_gate_matches(
        &gate, session_id, generation));

    /* SESSION_CLOSED can now occupy the same gate with no overlap. */
    assert(webrtc_cloud_callback_gate_try_begin(
        &gate, session_id, generation));
    webrtc_cloud_callback_gate_end(&gate, session_id, generation);
    assert(!gate.active);
}

int main(void)
{
    test_immediate_offer_and_candidate_are_deferred();
    test_rebuild_drops_only_stale_generation();
    test_close_waits_for_serial_callback_gate();
    puts("controlled session ownership policy: OK");
    return 0;
}
