/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* The browser's OS Services DataChannels: "guest-terminal" and
 * "guestfile_<drop>". The transport relays them to a handler that main
 * installs (OS Services). It hands over each channel's open, messages and
 * close, and sends what the handler has for the browser. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A message for the browser, or a channel to close. */
typedef struct {
    uint32_t channel;
    /* Close the channel instead of sending; everything before it was sent. */
    bool close;
    bool text;
    /* Valid until output_done(). */
    const uint8_t *data;
    size_t length;
} webrtc_transport_guest_output_t;

/* Every call comes from the peer task and must not block. channel is the
 * transport's ID for one channel: never 0, and not reused while the transport
 * runs. */
typedef struct {
    /* A channel with a guest label opened in the current session. false
     * closes it. */
    bool (*claim)(uint32_t channel, const char *label);
    /* A message of a claimed channel; data is valid only for the call. */
    void (*message)(uint32_t channel, bool text, const uint8_t *data,
                    size_t length);
    /* The last call for a claimed channel. The browser closed it, the
     * transport closed it for a close output or a failed send, or the peer
     * went away. */
    void (*closed)(uint32_t channel);
    /* The next output; false when there is none. output_done() follows,
     * before the next call. */
    bool (*next_output)(webrtc_transport_guest_output_t *output);
    /* sent is false when the browser's send buffer was full; the same output
     * then comes again. */
    void (*output_done)(uint32_t channel, bool sent);
} webrtc_transport_guest_channels_t;

/* Installs the handler, or removes it with NULL. Guest channels opened
 * without one are closed. Main calls it once, before
 * webrtc_transport_start(). */
void webrtc_transport_set_guest_channels(
    const webrtc_transport_guest_channels_t *channels);

/* The handler has output: wakes the peer task. From any task, also before
 * webrtc_transport_start(), when it does nothing. It does not wait. */
void webrtc_transport_guest_output_pending(void);

#ifdef __cplusplus
}
#endif
