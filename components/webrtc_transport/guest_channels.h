/* The OS Services DataChannels of one peer, on the peer task.
 *
 * Every function runs on the peer task: the esp_peer callbacks (open, data,
 * close), the send phase of its loop, and the peer's teardown. That task owns
 * the table, so the table has no lock. The handler is the copy the caller
 * took under the transport's lock. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "webrtc_guest_channels.h"

/* Four terminals and two file drops: what OS Services takes, and what is
 * left of the association's ten streams beside RPC and HID. */
#define GUEST_CHANNELS_MAX 6U
/* Bytes one send phase may take before the peer task returns to video and
 * HID input: one terminal message (OSVC_CHANNEL_MESSAGE_MAX). */
#define GUEST_CHANNELS_SEND_BUDGET (16U * 1024U)
/* What a message counts in that budget besides its bytes: its DTLS record,
 * SCTP chunk and sendto. On the Wi-Fi board a message of 316 bytes takes the
 * peer task 1.4 ms and one of 16 KiB 10.9 ms, so a message costs as much as
 * about 2 KiB of data. The terminal of a Windows host sends messages of
 * about 300 bytes: 7 of them fill a phase, which then takes about as long
 * as one message of 16 KiB. */
#define GUEST_CHANNELS_MESSAGE_COST 2048U
/* Outputs one send phase may take, closes and empty messages included. */
#define GUEST_CHANNELS_SEND_OUTPUTS_MAX 64U

typedef enum {
    GUEST_SEND_OK = 0,
    /* The send buffer is full; try the same message again later. */
    GUEST_SEND_BLOCKED,
    /* Not sent and never will be: the channel ends. */
    GUEST_SEND_FAILED,
} guest_send_result_t;

/* The peer, as the send phase uses it. */
typedef struct {
    guest_send_result_t (*send)(void *ctx, uint16_t stream_id, bool text,
                                const uint8_t *data, size_t length);
    /* Closes the stream. The peer's close callback may run inside, and
     * call guest_channels_closed(). */
    void (*close)(void *ctx, uint16_t stream_id);
    void *ctx;
} guest_peer_t;

typedef struct {
    /* 0 when the slot is free. */
    uint32_t channel;
    uint16_t stream_id;
} guest_slot_t;

typedef struct {
    uint32_t claimed;
    uint32_t refused;
    uint32_t closed;
    uint32_t messages_in;
    uint64_t bytes_in;
    uint32_t messages_out;
    uint64_t bytes_out;
    uint32_t blocked;
    uint32_t send_failures;
} guest_counters_t;

typedef struct {
    guest_slot_t slots[GUEST_CHANNELS_MAX];
    /* The last channel ID given out. */
    uint32_t last_channel;
    guest_counters_t counters;
} guest_channels_t;

/* "guest-terminal", or a label that starts "guestfile_". */
bool guest_channels_label(const char *label);

/* A channel with a guest label opened on stream_id. true when the handler
 * claimed it; the caller closes the stream otherwise. */
bool guest_channels_open(guest_channels_t *g,
                         const webrtc_transport_guest_channels_t *handler,
                         uint16_t stream_id, const char *label);

/* true when stream_id is a claimed channel: the handler has the message. */
bool guest_channels_message(guest_channels_t *g,
                            const webrtc_transport_guest_channels_t *handler,
                            uint16_t stream_id, bool text,
                            const uint8_t *data, size_t length);

/* The stream closed. true when it was a claimed channel. */
bool guest_channels_closed(guest_channels_t *g,
                           const webrtc_transport_guest_channels_t *handler,
                           uint16_t stream_id);

/* The peer went away: every claimed channel closed. */
void guest_channels_reset(guest_channels_t *g,
                          const webrtc_transport_guest_channels_t *handler);

/* Sends the handler's output, up to budget bytes. true when output may still
 * be waiting: the send buffer was full, or the budget ran out. */
bool guest_channels_send(guest_channels_t *g,
                         const webrtc_transport_guest_channels_t *handler,
                         const guest_peer_t *peer, size_t budget);
