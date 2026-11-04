/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "reboot_flow.h"

#include <string.h>

static const char s_will_reboot_event[] =
    "{\"jsonrpc\":\"2.0\",\"method\":\"willReboot\",\"params\":null}";

bool webrtc_reboot_slot_accept(
    webrtc_reboot_slot_t *slot, bool force, uint32_t session_id,
    uint32_t generation, uint16_t stream_id)
{
    if (!slot || slot->occupied || session_id == 0U || generation == 0U) {
        return false;
    }
    *slot = (webrtc_reboot_slot_t) {
        .occupied = true,
        .force = force,
        .session_id = session_id,
        .generation = generation,
        .stream_id = stream_id,
    };
    return true;
}

bool webrtc_reboot_slot_matches(
    const webrtc_reboot_slot_t *slot, uint32_t session_id,
    uint32_t generation, uint16_t stream_id)
{
    return slot && slot->occupied && slot->session_id == session_id &&
           slot->generation == generation && slot->stream_id == stream_id;
}

bool webrtc_reboot_slot_mark_dispatch_ready(
    webrtc_reboot_slot_t *slot, uint32_t session_id,
    uint32_t generation, uint16_t stream_id)
{
    if (!webrtc_reboot_slot_matches(
            slot, session_id, generation, stream_id) ||
        slot->dispatch_ready) {
        return false;
    }
    slot->dispatch_ready = true;
    return true;
}

bool webrtc_reboot_slot_cancel(webrtc_reboot_slot_t *slot)
{
    if (!slot || !slot->occupied || slot->dispatch_ready) {
        return false;
    }
    webrtc_reboot_slot_clear(slot);
    return true;
}

bool webrtc_reboot_slot_cancel_generation(
    webrtc_reboot_slot_t *slot, uint32_t generation)
{
    return slot && generation != 0U && slot->generation == generation &&
           webrtc_reboot_slot_cancel(slot);
}

void webrtc_reboot_slot_messages_wait(
    webrtc_reboot_slot_t *slot, bool wait, int64_t now_us)
{
    if (slot->messages_wait && !wait) {
        slot->sent_us = now_us;
    }
    slot->messages_wait = wait;
}

bool webrtc_reboot_slot_message_lost(
    webrtc_reboot_slot_t *slot, uint16_t stream_id, uint32_t number)
{
    bool reboot_message = slot->dispatch_ready &&
        stream_id == slot->stream_id &&
        (int32_t)(number - slot->first_queued) >= 0 &&
        (int32_t)(number - slot->last_queued) <= 0;
    if (reboot_message) {
        webrtc_reboot_slot_clear(slot);
    }
    return reboot_message;
}

int64_t webrtc_reboot_wait_us(const webrtc_reboot_slot_t *slot,
                              int64_t now_us)
{
    if (!slot->messages_wait) {
        int64_t settle_us = WEBRTC_REBOOT_SETTLE_US - (now_us - slot->sent_us);
        return settle_us > 0 ? settle_us : 0;
    }
    int64_t limit_us =
        WEBRTC_REBOOT_LIMIT_US - (now_us - slot->dispatched_us);
    if (limit_us <= 0) {
        return 0;
    }
    return limit_us < WEBRTC_REBOOT_POLL_US ? limit_us : WEBRTC_REBOOT_POLL_US;
}

void webrtc_reboot_slot_clear(webrtc_reboot_slot_t *slot)
{
    if (slot) {
        *slot = (webrtc_reboot_slot_t) { 0 };
    }
}

bool webrtc_reboot_render_will_event(
    char *output, size_t output_capacity, size_t *output_size)
{
    if (!output || !output_size || output_capacity < sizeof(s_will_reboot_event)) {
        if (output_size) {
            *output_size = 0;
        }
        return false;
    }
    memcpy(output, s_will_reboot_event, sizeof(s_will_reboot_event));
    *output_size = sizeof(s_will_reboot_event) - 1U;
    return true;
}
