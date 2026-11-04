/* Messages for the browser that the SCTP send buffer cannot take yet.
 *
 * esp_peer refuses a message with ESP_PEER_ERR_WOULD_BLOCK while its send
 * buffer holds data the browser has not acknowledged; with packet loss and
 * bulk output that can last a while. A message refused that way waits here
 * and goes when the buffer drains. Each stream keeps its order: a message
 * waits while an earlier one for its stream waits. Streams do not wait for
 * each other, so a small HIDRPC message passes a large RPC answer.
 *
 * Only the peer task uses a queue, so it has no lock. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* At most this many bytes wait, headers included: the send buffer's size. */
#define DATA_CHANNEL_QUEUE_MAX_BYTES (256U * 1024U)

typedef enum {
    DATA_CHANNEL_SENT = 0,
    /* The send buffer is full: the message can go later. */
    DATA_CHANNEL_BLOCKED,
    /* The message is lost. */
    DATA_CHANNEL_FAILED,
    /* data_channel_queue_send() only: the message waits in the queue. */
    DATA_CHANNEL_QUEUED,
} data_channel_send_result_t;

typedef data_channel_send_result_t (*data_channel_send_fn_t)(
    void *ctx, uint16_t stream_id, bool text, const uint8_t *data,
    size_t size);

typedef struct data_channel_message data_channel_message_t;

typedef struct {
    data_channel_message_t *head;
    data_channel_message_t *tail;
    /* The messages' allocations, headers included. */
    size_t bytes;
    /* Messages queued so far; each message keeps its number. */
    uint32_t queued;
} data_channel_queue_t;

/* Sends the message, or queues a copy when an earlier message for its
 * stream waits or the send buffer is full. FAILED when the send failed for
 * another reason, or the queue cannot take the copy. */
data_channel_send_result_t data_channel_queue_send(
    data_channel_queue_t *queue, data_channel_send_fn_t send, void *ctx,
    uint16_t stream_id, bool text, const uint8_t *data, size_t size);

/* Learns the stream and number of a message that a drain dropped. */
typedef void (*data_channel_lost_fn_t)(void *ctx, uint16_t stream_id,
                                       uint32_t number);

/* Sends what waits, in order for each stream, until the send buffer is
 * full. A message that fails is dropped, and lost (when not NULL) learns
 * of it; lost may drop other messages. Returns the number that failed. */
size_t data_channel_queue_drain(data_channel_queue_t *queue,
                                data_channel_send_fn_t send,
                                data_channel_lost_fn_t lost, void *ctx);

/* Whether a message for the stream with a number up to last still waits. */
bool data_channel_queue_holds(const data_channel_queue_t *queue,
                              uint16_t stream_id, uint32_t last);

/* The stream's messages with a number up to last are dropped unsent. */
void data_channel_queue_drop_through(data_channel_queue_t *queue,
                                     uint16_t stream_id, uint32_t last);

/* The stream closed: its messages are dropped. */
void data_channel_queue_drop_stream(data_channel_queue_t *queue,
                                    uint16_t stream_id);

/* The peer closed: every message is dropped. */
void data_channel_queue_clear(data_channel_queue_t *queue);
