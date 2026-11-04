#include "data_channel_queue.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* PSRAM: internal RAM is short, and a waiting message is not urgent. */
#define QUEUE_ALLOC(size) \
    heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define QUEUE_FREE(memory) heap_caps_free(memory)
#else
#define QUEUE_ALLOC(size) malloc(size)
#define QUEUE_FREE(memory) free(memory)
#endif

/* Streams one drain can find blocked; it stops at the next one. */
#define BLOCKED_STREAMS_MAX 8U

struct data_channel_message {
    data_channel_message_t *next;
    size_t size;
    uint32_t number;
    uint16_t stream_id;
    bool text;
    uint8_t data[];
};

static bool stream_waits(const data_channel_queue_t *queue,
                         uint16_t stream_id)
{
    for (const data_channel_message_t *message = queue->head; message;
         message = message->next) {
        if (message->stream_id == stream_id) {
            return true;
        }
    }
    return false;
}

static bool enqueue(data_channel_queue_t *queue, uint16_t stream_id,
                    bool text, const uint8_t *data, size_t size)
{
    const size_t bytes = sizeof(data_channel_message_t) + size;
    if (size > DATA_CHANNEL_QUEUE_MAX_BYTES ||
        bytes > DATA_CHANNEL_QUEUE_MAX_BYTES - queue->bytes) {
        return false;
    }
    data_channel_message_t *message = QUEUE_ALLOC(bytes);
    if (message == NULL) {
        return false;
    }
    *message = (data_channel_message_t){
        .size = size,
        .number = ++queue->queued,
        .stream_id = stream_id,
        .text = text,
    };
    memcpy(message->data, data, size);
    if (queue->tail) {
        queue->tail->next = message;
    } else {
        queue->head = message;
    }
    queue->tail = message;
    queue->bytes += bytes;
    return true;
}

/* Removes message, which follows previous (NULL for the head); returns the
 * message after it. */
static data_channel_message_t *dequeue(data_channel_queue_t *queue,
                                       data_channel_message_t *previous,
                                       data_channel_message_t *message)
{
    data_channel_message_t *next = message->next;
    if (previous) {
        previous->next = next;
    } else {
        queue->head = next;
    }
    if (queue->tail == message) {
        queue->tail = previous;
    }
    queue->bytes -= sizeof(*message) + message->size;
    QUEUE_FREE(message);
    return next;
}

data_channel_send_result_t data_channel_queue_send(
    data_channel_queue_t *queue, data_channel_send_fn_t send, void *ctx,
    uint16_t stream_id, bool text, const uint8_t *data, size_t size)
{
    data_channel_send_result_t result =
        stream_waits(queue, stream_id) ?
            DATA_CHANNEL_BLOCKED : send(ctx, stream_id, text, data, size);
    if (result != DATA_CHANNEL_BLOCKED) {
        return result;
    }
    return enqueue(queue, stream_id, text, data, size) ?
               DATA_CHANNEL_QUEUED : DATA_CHANNEL_FAILED;
}

static bool listed(const uint16_t *streams, size_t count, uint16_t stream_id)
{
    for (size_t i = 0; i < count; ++i) {
        if (streams[i] == stream_id) {
            return true;
        }
    }
    return false;
}

size_t data_channel_queue_drain(data_channel_queue_t *queue,
                                data_channel_send_fn_t send,
                                data_channel_lost_fn_t lost, void *ctx)
{
    uint16_t blocked[BLOCKED_STREAMS_MAX];
    size_t blocked_count = 0U;
    size_t dropped = 0U;
    data_channel_message_t *previous = NULL;
    data_channel_message_t *message = queue->head;
    while (message) {
        if (listed(blocked, blocked_count, message->stream_id)) {
            previous = message;
            message = message->next;
            continue;
        }
        data_channel_send_result_t result =
            send(ctx, message->stream_id, message->text, message->data,
                 message->size);
        if (result == DATA_CHANNEL_BLOCKED) {
            if (blocked_count == BLOCKED_STREAMS_MAX) {
                break;
            }
            blocked[blocked_count++] = message->stream_id;
            previous = message;
            message = message->next;
            continue;
        }
        if (result == DATA_CHANNEL_SENT) {
            message = dequeue(queue, previous, message);
            continue;
        }
        const uint16_t stream_id = message->stream_id;
        const uint32_t number = message->number;
        dequeue(queue, previous, message);
        dropped++;
        if (lost) {
            lost(ctx, stream_id, number);
        }
        /* lost may have dropped other messages. Each message before this
         * one is on a blocked stream, so starting over sends none twice. */
        previous = NULL;
        message = queue->head;
    }
    return dropped;
}

bool data_channel_queue_holds(const data_channel_queue_t *queue,
                              uint16_t stream_id, uint32_t last)
{
    /* A stream's messages wait in order: its first one is its oldest. */
    for (const data_channel_message_t *message = queue->head; message;
         message = message->next) {
        if (message->stream_id == stream_id) {
            return (int32_t)(message->number - last) <= 0;
        }
    }
    return false;
}

void data_channel_queue_drop_through(data_channel_queue_t *queue,
                                     uint16_t stream_id, uint32_t last)
{
    data_channel_message_t *previous = NULL;
    data_channel_message_t *message = queue->head;
    while (message) {
        if (message->stream_id == stream_id &&
            (int32_t)(message->number - last) <= 0) {
            message = dequeue(queue, previous, message);
        } else {
            previous = message;
            message = message->next;
        }
    }
}

void data_channel_queue_drop_stream(data_channel_queue_t *queue,
                                    uint16_t stream_id)
{
    data_channel_queue_drop_through(queue, stream_id, queue->queued);
}

void data_channel_queue_clear(data_channel_queue_t *queue)
{
    while (queue->head) {
        dequeue(queue, NULL, queue->head);
    }
}
