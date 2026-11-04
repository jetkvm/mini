/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The browser's data channels of the terminal and file services, between the
 * WebRTC transport and the session.
 *
 * Two tasks use a channel table. The transport (the peer task) claims a
 * channel when it opens, hands over its messages and its close, and takes
 * the messages to send. The service task opens and closes terminals and
 * drops in the session, feeds it the browser's bytes, and receives the
 * session's terminal output, terminal ends, credit and outcomes through the
 * hooks below. Neither side waits for the other: each copies under the
 * table's lock (the platform's lock and unlock) and the service task never
 * holds it while it calls the session, whose writes can block for 2 seconds.
 *
 * Buffers come from the platform when a channel opens and go back when it
 * is gone. The browser cannot be slowed down once its bytes have arrived,
 * so each buffer has a fixed size: a terminal whose keystrokes overflow it
 * closes. Shell output that does not fit waits in the session, which stops
 * reading USB until the browser has taken enough (the session's rule). A
 * drop's bytes cannot overflow: the browser may only send what it has
 * credit for, and the table gives it the sender's credit only as far as the
 * buffer has room.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os_services_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Terminals and two drop channels: the last drop's channel may still be
 * closing when the next one opens. */
#define OSVC_CHANNELS_MAX (OSVC_TERMINAL_SESSIONS_MAX + 2U)
/* Keystrokes a terminal channel holds while the IN endpoint is busy. */
#define OSVC_TERMINAL_INPUT_BYTES (64U * 1024U)
/* Shell output waiting for the browser: the sender's whole window. */
#define OSVC_TERMINAL_OUTPUT_BYTES OSVC_TERMINAL_OUTPUT_WINDOW
/* The largest message to the browser. */
#define OSVC_CHANNEL_MESSAGE_MAX (16U * 1024U)
/* Output the browser took goes back to the sender as terminal.credit in
 * steps of at least this much. */
#define OSVC_TERMINAL_CREDIT_STEP OSVC_CHANNEL_MESSAGE_MAX
/* Credit a drop's browser may hold and the bytes not yet relayed,
 * together. */
#define OSVC_FILE_WINDOW_BYTES (32U * 1024U)
/* Credit a drop's browser may hold at once. What it sends at once waits in
 * the Mini's receive queue ahead of key presses and pointer moves, about
 * 2 us a byte: 32 KiB held the moves for up to 80 ms, and a macOS guest,
 * whose screen changes only when the pointer moves, showed 23 fps. */
#define OSVC_FILE_FLIGHT_BYTES (8U * 1024U)
/* The window and room for the framing of the messages it arrives in, 8
 * bytes each. A browser sends no message under 1 KiB that leaves credit
 * unused, which keeps the framing well within that room. */
#define OSVC_FILE_INPUT_BYTES (OSVC_FILE_WINDOW_BYTES + 32U * 1024U)
/* Credit goes to the browser in steps of at least this much, unless it is
 * the rest of the sender's credit: few and large chunks come back. */
#define OSVC_FILE_GRANT_MIN (8U * 1024U)
/* A finished drop's channel closes this long after its last outcome, when
 * the browser has not closed it by then. */
#define OSVC_FILE_CLOSE_DELAY_MS 500
/* A longer final path is left out of the outcome. */
#define OSVC_FILE_PATH_REPORT_MAX 1024U

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    /* A channel's buffers; NULL when there is no memory for them. */
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *memory);
    int64_t (*now_ms)(void *ctx);
    /* The service task has work: a channel opened, closed or has input.
     * From the transport's side; must not block. */
    void (*service_pending)(void *ctx);
    /* The transport has messages to send or a channel to close. From the
     * service task (and the transport's side); must not block. */
    void (*output_pending)(void *ctx);
} osvc_channels_platform_t;

/* What the transport sends next on one of its channels. */
typedef struct {
    uint32_t channel;
    /* Close the channel, after everything sent before. */
    bool close;
    bool text;
    /* Valid until osvc_channels_output_done(). */
    const uint8_t *data;
    size_t length;
} osvc_channel_output_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t used;
} osvc_byte_ring_t;

/* One transfer of a drop, as its channel reports it. */
typedef struct {
    /* The sender's credit the browser has not been given yet. */
    uint64_t pending;
    /* Credit the browser was given and has not used. */
    uint64_t unused;
    /* What is left of the file's size for credit: credit beyond it could
     * never be used and would only shrink the drop's window. */
    uint64_t room;
    /* The final path while the outcome waits to be sent. */
    char *path;
    uint16_t path_length;
    uint8_t status;
    /* The outcome arrived; whether it went to the browser. */
    bool done;
    bool reported;
    /* The browser asked to cancel. */
    bool cancel;
} osvc_channel_transfer_t;

typedef enum {
    OSVC_CHANNEL_FREE = 0,
    OSVC_CHANNEL_TERMINAL,
    OSVC_CHANNEL_FILE,
    /* A drop's channel without memory for its buffers: the service task
     * ends the drop, and the channel sends a failed outcome for each of its
     * transfers, then closes. */
    OSVC_CHANNEL_REFUSED,
} osvc_channel_kind_t;

typedef struct {
    uint32_t terminal_id;
    /* The session's terminal is open: closing the channel closes it. */
    bool live;
    bool resize;
    uint16_t rows;
    uint16_t cols;
    /* "\r\n[reason]\r\n", sent before the close; empty for none. The
     * longest reason, a fault, is 24 + 95 bytes. */
    char closing[160];
    size_t closing_length;
    bool closing_sent;
    /* Output the browser took that the sender has no credit for yet. */
    uint32_t drained;
    osvc_byte_ring_t input;
    osvc_byte_ring_t output;
} osvc_terminal_channel_t;

typedef struct {
    uint32_t drop_id;
    /* The session took the channel for its drop. */
    bool attached;
    /* Credit the browser holds that its bytes have not used yet: the sum of
     * the transfers' unused. */
    uint64_t outstanding;
    /* When the channel closes (0: not yet decided). */
    int64_t close_at;
    /* Where the next search for a message starts. */
    uint32_t next_transfer;
    /* The message out with the transport, and what it reports. */
    uint32_t message_transfer;
    uint64_t message_grant;
    bool message_done;
    size_t message_length;
    char message[OSVC_FILE_PATH_REPORT_MAX * 6U + 128U];
    /* Framed messages: transfer ID u32, length u32, bytes. */
    osvc_byte_ring_t input;
    osvc_channel_transfer_t transfers[OSVC_FILE_DROP_MAX];
} osvc_file_channel_t;

typedef struct {
    osvc_channel_kind_t kind;
    /* The transport's ID for the channel. */
    uint32_t channel;
    /* The service task opened it in the session. */
    bool opened;
    /* The transport reported its close: nothing more goes either way. */
    bool closed;
    /* The service task ended it: what is queued goes, then the close. */
    bool ending;
    /* The close went to the transport. */
    bool close_sent;
    /* An output is out with the transport. */
    bool output_busy;
    /* The service task is done with it; the slot is freed when the
     * transport gives the output back. */
    bool release;
    /* The browser's input did not fit. */
    bool overflow;
    /* What the output out with the transport is, and its length. */
    uint8_t output_kind;
    size_t output_length;
    /* OSVC_CHANNEL_REFUSED: the drop, its count of transfers (known once
     * the service task ended it), the outcomes sent, and the one out with
     * the transport. */
    uint32_t refused_drop;
    uint32_t refused_count;
    uint32_t refused_sent;
    char refused_message[64];
    size_t refused_length;
    /* Either part, in one allocation. */
    union {
        osvc_terminal_channel_t *terminal;
        osvc_file_channel_t *file;
        void *memory;
    };
} osvc_channel_t;

typedef struct {
    uint64_t claimed;
    uint64_t refused;
    uint64_t no_memory;
    uint64_t input_overflows;
    uint64_t messages_in;
    uint64_t messages_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
} osvc_channels_counters_t;

typedef struct {
    osvc_channels_platform_t platform;
    osvc_channel_t channels[OSVC_CHANNELS_MAX];
    /* Where the next search for output, and the next service, starts. */
    size_t next_output;
    size_t next_service;
    /* A drop whose channel found the table full: the service task ends it,
     * so that it does not wait for a channel that is gone. */
    uint32_t refused_drop;
    osvc_channels_counters_t counters;
} osvc_channels_t;

void osvc_channels_init(osvc_channels_t *channels,
                        const osvc_channels_platform_t *platform);

/*
 * The transport's side. channel is the transport's ID for the channel, never
 * 0 and not reused while the channel is known.
 */

/* A channel opened. Returns true when it is a terminal or drop channel and
 * the table takes it; the transport closes any other. */
bool osvc_channels_claim(osvc_channels_t *channels, uint32_t channel,
                         const char *label);
/* A message of a claimed channel; data is only valid for the call. */
void osvc_channels_message(osvc_channels_t *channels, uint32_t channel,
                           bool text, const uint8_t *data, size_t length);
/* A claimed channel closed, from either side, or the peer went away. The
 * last call for the channel. */
void osvc_channels_closed(osvc_channels_t *channels, uint32_t channel);
/* The next message to send or channel to close. Returns false when there
 * is none. Then osvc_channels_output_done() before any other output. */
bool osvc_channels_next_output(osvc_channels_t *channels,
                               osvc_channel_output_t *output);
/* sent is false when the transport could not send it now (its buffer is
 * full); the same output then comes again. */
void osvc_channels_output_done(osvc_channels_t *channels, uint32_t channel,
                               bool sent);

/*
 * The service task's side.
 */

/* Opens, feeds and closes in the session what the transport handed over.
 * Returns true when work is left for the next call. */
bool osvc_channels_service(osvc_channels_t *channels, osvc_session_t *session);

/* The session's hooks (osvc_platform_t). */
bool osvc_channels_terminal_output(osvc_channels_t *channels,
                                   uint32_t terminal_id, const uint8_t *data,
                                   size_t length);
void osvc_channels_terminal_closed(osvc_channels_t *channels,
                                   uint32_t terminal_id, const char *reason);
void osvc_channels_file_credit(osvc_channels_t *channels, uint32_t drop_id,
                               uint32_t transfer_id, uint64_t bytes);
void osvc_channels_file_done(osvc_channels_t *channels, uint32_t drop_id,
                             uint32_t transfer_id, osvc_file_status_t status,
                             const char *path, size_t path_length, bool last);

/* Parses {"rows":R,"cols":C} (the terminal's size) and
 * {"type":"cancel","transfer":N}: JSON objects that may hold other members.
 * Exposed for the tests. */
bool osvc_channels_parse_size(const uint8_t *text, size_t length,
                              uint32_t *rows, uint32_t *cols);
bool osvc_channels_parse_cancel(const uint8_t *text, size_t length,
                                uint32_t *transfer_id);

#ifdef __cplusplus
}
#endif
