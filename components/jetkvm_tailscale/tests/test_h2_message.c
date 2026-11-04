#include "ml_h2_message.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned delivered, frames;
static bool receive_message(const uint8_t *message, size_t length, void *ctx)
{
    (void)ctx;
    assert(length == 2 && !strcmp((const char *)message, "{}"));
    ++delivered;
    return true;
}
static bool receive_frame(uint8_t type, uint8_t flags, uint32_t stream,
    const uint8_t *payload, size_t length, void *ctx)
{
    (void)type; (void)flags; (void)stream; (void)payload; (void)length; (void)ctx;
    ++frames;
    return true;
}

void test_h2_message(void)
{
    /* Streaming HEADERS, unrelated stream DATA, then a JSON length prefix
     * split across two DATA frames. No frame has END_STREAM. */
    const uint8_t capture[] = {
        0,0,1,1,4,0,0,0,3,0x88,
        0,0,2,0,1,0,0,0,1,'{','}',
        0,0,2,0,0,0,0,0,3,2,0,
        0,0,4,0,0,0,0,0,3,0,0,'{','}',
    };
    uint8_t message[32];
    size_t size = 99;
    for (size_t n = 0; n < sizeof(capture); ++n)
        assert(ml_h2_first_message(capture, n, 3, message, sizeof(message), &size) == 0 && size == 0);
    assert(ml_h2_first_message(capture, sizeof(capture), 3, message, sizeof(message), &size) == 1);
    assert(size == 2 && !strcmp((char *)message, "{}"));
    assert(ml_h2_first_message(capture, sizeof(capture), 3, message, 2, &size) == -1);
    const uint8_t padded[] = {0,0,9,0,8,0,0,0,3,2,2,0,0,0,'{','}',0,0};
    assert(ml_h2_first_message(padded, sizeof(padded), 3, message, sizeof(message), &size) == 1);
    assert(size == 2 && !strcmp((char *)message, "{}"));
    // The registration reply's DATA frames lose a pad length byte and padding.
    const uint8_t padded_data[] = {2,'{','}',0,0};
    const uint8_t *data = padded_data;
    size_t data_size = sizeof(padded_data);
    assert(ml_h2_data_payload(8, &data, &data_size) && data_size == 2 && !memcmp(data, "{}", 2));
    data = padded_data;
    data_size = sizeof(padded_data);
    assert(ml_h2_data_payload(0, &data, &data_size) && data == padded_data && data_size == 5);
    data_size = 2;
    assert(!ml_h2_data_payload(8, &data, &data_size));
    const uint8_t closed[] = {0,0,1,1,5,0,0,0,3,0x88};
    assert(ml_h2_first_message(closed, sizeof(closed), 3, message, sizeof(message), &size) == -1);
    const uint8_t oversized[] = {0,0,4,0,0,0,0,0,3,255,255,255,255};
    assert(ml_h2_first_message(oversized, sizeof(oversized), 3, message, sizeof(message), &size) == -1);
    const uint8_t bad_padding[] = {0,0,1,0,8,0,0,0,3,2};
    assert(ml_h2_first_message(bad_padding, sizeof(bad_padding), 3, message, sizeof(message), &size) == -1);
    // Two messages in one frame follow a prefix split across DATA frames.
    // Transport records split the H2 header and payload at every possible byte.
    const uint8_t updates[] = {
        0,0,0,4,0,0,0,0,0,
        0,0,2,0,0,0,0,0,3,2,0,
        0,0,10,0,0,0,0,0,3,0,0,'{','}',2,0,0,0,'{','}',
    };
    uint8_t frame[32];
    ml_h2_stream_t stream = { .stream = 3, .frame = frame, .frame_capacity = sizeof(frame),
        .message = message, .message_capacity = sizeof(message) };
    for (size_t split = 0; split <= sizeof(updates); ++split) {
        ml_h2_stream_reset(&stream);
        delivered = frames = 0;
        assert(ml_h2_stream_feed(&stream, updates, split, receive_frame, receive_message, NULL));
        assert(ml_h2_stream_feed(&stream, updates + split, sizeof(updates) - split,
            receive_frame, receive_message, NULL));
        assert(delivered == 2 && frames == 3);
    }
    ml_h2_stream_reset(&stream);
    delivered = frames = 0;
    for (size_t i = 0; i < sizeof(updates); ++i)
        assert(ml_h2_stream_feed(&stream, updates + i, 1, receive_frame, receive_message, NULL));
    assert(delivered == 2 && frames == 3);
    ml_h2_stream_reset(&stream);
    assert(ml_h2_stream_feed(&stream, padded, sizeof(padded), receive_frame, receive_message, NULL));
    ml_h2_stream_reset(&stream);
    assert(!ml_h2_stream_feed(&stream, bad_padding, sizeof(bad_padding), receive_frame, receive_message, NULL));
    ml_h2_stream_reset(&stream);
    assert(!ml_h2_stream_feed(&stream, oversized, sizeof(oversized), receive_frame, receive_message, NULL));
    ml_h2_stream_reset(&stream);
    assert(!ml_h2_stream_feed(&stream, closed, sizeof(closed), receive_frame, receive_message, NULL));
    puts("Streaming map: fragmented prefix, stream isolation, padding and bounded payload: PASS");
}
