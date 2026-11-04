/* The OS Services DataChannels of one peer, on the peer task. See
 * guest_channels.h. */
#include "guest_channels.h"

#include <string.h>

#define TERMINAL_LABEL "guest-terminal"
#define FILE_LABEL_PREFIX "guestfile_"

bool guest_channels_label(const char *label)
{
    return label != NULL &&
           (strcmp(label, TERMINAL_LABEL) == 0 ||
            strncmp(label, FILE_LABEL_PREFIX, sizeof(FILE_LABEL_PREFIX) - 1U) == 0);
}

static guest_slot_t *find_stream(guest_channels_t *g, uint16_t stream_id)
{
    for (size_t i = 0U; i < GUEST_CHANNELS_MAX; ++i) {
        if (g->slots[i].channel != 0U && g->slots[i].stream_id == stream_id) {
            return &g->slots[i];
        }
    }
    return NULL;
}

static guest_slot_t *find_channel(guest_channels_t *g, uint32_t channel)
{
    for (size_t i = 0U; i < GUEST_CHANNELS_MAX; ++i) {
        if (channel != 0U && g->slots[i].channel == channel) {
            return &g->slots[i];
        }
    }
    return NULL;
}

/* Frees the slot first: the handler may wake the peer task, never call back
 * into the table, but the slot is free either way. */
static void close_slot(guest_channels_t *g,
                       const webrtc_transport_guest_channels_t *handler,
                       guest_slot_t *slot)
{
    const uint32_t channel = slot->channel;
    slot->channel = 0U;
    slot->stream_id = 0U;
    g->counters.closed++;
    if (handler->closed != NULL) {
        handler->closed(channel);
    }
}

static uint32_t next_channel(guest_channels_t *g)
{
    for (;;) {
        const uint32_t channel = ++g->last_channel;
        if (channel != 0U && find_channel(g, channel) == NULL) {
            return channel;
        }
    }
}

bool guest_channels_open(guest_channels_t *g,
                         const webrtc_transport_guest_channels_t *handler,
                         uint16_t stream_id, const char *label)
{
    /* A stream that opens again closed without our knowing. */
    guest_slot_t *stale = find_stream(g, stream_id);
    if (stale != NULL) {
        close_slot(g, handler, stale);
    }
    guest_slot_t *slot = NULL;
    for (size_t i = 0U; i < GUEST_CHANNELS_MAX && slot == NULL; ++i) {
        if (g->slots[i].channel == 0U) {
            slot = &g->slots[i];
        }
    }
    if (slot == NULL || handler->claim == NULL ||
        !guest_channels_label(label)) {
        g->counters.refused++;
        return false;
    }
    const uint32_t channel = next_channel(g);
    if (!handler->claim(channel, label)) {
        g->counters.refused++;
        return false;
    }
    slot->channel = channel;
    slot->stream_id = stream_id;
    g->counters.claimed++;
    return true;
}

bool guest_channels_message(guest_channels_t *g,
                            const webrtc_transport_guest_channels_t *handler,
                            uint16_t stream_id, bool text,
                            const uint8_t *data, size_t length)
{
    guest_slot_t *slot = find_stream(g, stream_id);
    if (slot == NULL) {
        return false;
    }
    g->counters.messages_in++;
    g->counters.bytes_in += length;
    if (handler->message != NULL) {
        handler->message(slot->channel, text, data, length);
    }
    return true;
}

bool guest_channels_closed(guest_channels_t *g,
                           const webrtc_transport_guest_channels_t *handler,
                           uint16_t stream_id)
{
    guest_slot_t *slot = find_stream(g, stream_id);
    if (slot == NULL) {
        return false;
    }
    close_slot(g, handler, slot);
    return true;
}

void guest_channels_reset(guest_channels_t *g,
                          const webrtc_transport_guest_channels_t *handler)
{
    for (size_t i = 0U; i < GUEST_CHANNELS_MAX; ++i) {
        if (g->slots[i].channel != 0U) {
            close_slot(g, handler, &g->slots[i]);
        }
    }
}

/* Closes the channel's stream. The peer reports the close inside the call
 * when it can; when it did not, the channel is closed here. */
static void close_stream(guest_channels_t *g,
                         const webrtc_transport_guest_channels_t *handler,
                         const guest_peer_t *peer, uint32_t channel)
{
    guest_slot_t *slot = find_channel(g, channel);
    if (slot == NULL) {
        return;
    }
    peer->close(peer->ctx, slot->stream_id);
    slot = find_channel(g, channel);
    if (slot != NULL) {
        close_slot(g, handler, slot);
    }
}

bool guest_channels_send(guest_channels_t *g,
                         const webrtc_transport_guest_channels_t *handler,
                         const guest_peer_t *peer, size_t budget)
{
    if (handler->next_output == NULL || handler->output_done == NULL) {
        return false;
    }
    size_t sent = 0U;
    for (unsigned outputs = 0U; outputs < GUEST_CHANNELS_SEND_OUTPUTS_MAX;
         ++outputs) {
        if (sent >= budget) {
            return true;
        }
        webrtc_transport_guest_output_t output;
        if (!handler->next_output(&output)) {
            return false;
        }
        const guest_slot_t *slot = find_channel(g, output.channel);
        if (slot == NULL || output.close || output.length == 0U) {
            /* A channel already closed has nothing left to send, and an
             * empty message is nothing to send. */
            handler->output_done(output.channel, true);
            if (slot != NULL && output.close) {
                close_stream(g, handler, peer, output.channel);
            }
            continue;
        }
        const guest_send_result_t result =
            peer->send(peer->ctx, slot->stream_id, output.text, output.data,
                       output.length);
        if (result == GUEST_SEND_BLOCKED) {
            handler->output_done(output.channel, false);
            g->counters.blocked++;
            return true;
        }
        handler->output_done(output.channel, true);
        if (result != GUEST_SEND_OK) {
            /* A lost message would corrupt what follows: end the channel. */
            g->counters.send_failures++;
            close_stream(g, handler, peer, output.channel);
            continue;
        }
        g->counters.messages_out++;
        g->counters.bytes_out += output.length;
        sent += output.length + GUEST_CHANNELS_MESSAGE_COST;
    }
    return true;
}
