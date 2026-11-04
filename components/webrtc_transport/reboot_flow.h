/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * One accepted reboot request.  This is intentionally a slot rather than a
 * queue: after accepting a reboot, every later request must receive a busy
 * error until the machine restarts (or transport teardown cancels the slot).
 */
typedef struct {
    bool occupied;
    bool dispatch_ready;
    bool force;
    uint32_t session_id;
    uint32_t generation;
    uint16_t stream_id;
    /* Set at dispatch: when, and the send queue's numbers
     * (data_channel_queue_t.queued) that the response and willReboot have
     * if they wait in it: from first_queued up to last_queued. */
    int64_t dispatched_us;
    uint32_t first_queued;
    uint32_t last_queued;
    /* The response or willReboot still waits in the send queue. */
    bool messages_wait;
    /* When both left the send queue (the dispatch when neither waited). */
    int64_t sent_us;
} webrtc_reboot_slot_t;

/* A dispatched reboot restarts the Mini this long after the response and
 * willReboot left the send queue, so they can reach the browser... */
#define WEBRTC_REBOOT_SETTLE_US (1000LL * 1000LL)
/* ...or this long after the dispatch while they still wait in it for a
 * browser that does not acknowledge. */
#define WEBRTC_REBOOT_LIMIT_US (2000LL * 1000LL)
/* How often the reboot task looks whether they left the queue. */
#define WEBRTC_REBOOT_POLL_US (10LL * 1000LL)

/** Atomically protected by the transport lock at every call site. */
bool webrtc_reboot_slot_accept(
    webrtc_reboot_slot_t *slot, bool force, uint32_t session_id,
    uint32_t generation, uint16_t stream_id);

/** Match the exact peer request that owns the single slot. */
bool webrtc_reboot_slot_matches(
    const webrtc_reboot_slot_t *slot, uint32_t session_id,
    uint32_t generation, uint16_t stream_id);

/**
 * Release the reboot task only after the RPC response and willReboot send
 * attempts have both completed on the owning peer task.
 */
bool webrtc_reboot_slot_mark_dispatch_ready(
    webrtc_reboot_slot_t *slot, uint32_t session_id,
    uint32_t generation, uint16_t stream_id);

/**
 * Cancel a request that is not dispatched yet. A dispatched reboot was
 * promised to the browser and always happens; only the transport's stop
 * clears it.
 */
bool webrtc_reboot_slot_cancel(webrtc_reboot_slot_t *slot);

/** Cancel only a request owned by the generation being invalidated. */
bool webrtc_reboot_slot_cancel_generation(
    webrtc_reboot_slot_t *slot, uint32_t generation);

/** The send queue changed: whether the response or willReboot still wait. */
void webrtc_reboot_slot_messages_wait(
    webrtc_reboot_slot_t *slot, bool wait, int64_t now_us);

/**
 * The send queue dropped a message whose send failed. When it was the
 * dispatched reboot's response or willReboot, the browser was not told, so
 * the reboot is cancelled. Returns whether it was.
 */
bool webrtc_reboot_slot_message_lost(
    webrtc_reboot_slot_t *slot, uint16_t stream_id, uint32_t number);

/** How long a dispatched reboot still waits; 0 restarts the Mini now. */
int64_t webrtc_reboot_wait_us(const webrtc_reboot_slot_t *slot,
                              int64_t now_us);

void webrtc_reboot_slot_clear(webrtc_reboot_slot_t *slot);

/** Render the Go-compatible server notification with an explicit null param. */
bool webrtc_reboot_render_will_event(
    char *output, size_t output_capacity, size_t *output_size);
