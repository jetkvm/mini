#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The data of a DATA frame without its PADDED (0x08) pad length byte and
 * padding. False when the pad length does not fit the frame. */
bool ml_h2_data_payload(uint8_t flags, const uint8_t **data, size_t *size);

/* Extract the first length-prefixed control message on a streaming H2
 * response. Return 1 for a complete message, 0 for more input, -1 on error. */
int ml_h2_first_message(const uint8_t *frames, size_t length, uint32_t stream,
                        uint8_t *message, size_t capacity, size_t *message_length);

typedef struct {
    uint8_t header[9], prefix[4];
    size_t header_length, frame_length, frame_received;
    size_t prefix_length, message_length, message_received;
    uint8_t *frame, *message;
    size_t frame_capacity, message_capacity;
    uint32_t stream;
} ml_h2_stream_t;
typedef bool (*ml_h2_frame_callback_t)(uint8_t type, uint8_t flags, uint32_t stream,
    const uint8_t *payload, size_t length, void *ctx);
typedef bool (*ml_h2_message_callback_t)(const uint8_t *message, size_t length, void *ctx);
void ml_h2_stream_reset(ml_h2_stream_t *state);
bool ml_h2_stream_feed(ml_h2_stream_t *state, const uint8_t *data, size_t length,
    ml_h2_frame_callback_t frame_callback, ml_h2_message_callback_t message_callback, void *ctx);
