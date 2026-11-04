#include "ml_derp_stream.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A fragmented PING followed by a coalesced, empty KeepAlive frame. */
static const uint8_t incoming[] = {
    0x12, 0, 0, 0, 8, 'r', 'e', 'l', 'a', 'y', 'p', 'i', 'n',
    0x06, 0, 0, 0, 0,
};

typedef struct {
    uint64_t now;
    size_t read_used;
    uint8_t output[2048];
    size_t output_used;
    uint8_t *retry_buffer;
    size_t retry_len;
    bool read_wait;
} peer_t;

static int peer_read(void *context, uint8_t *data, size_t len) {
    peer_t *peer = context;
    peer->read_wait = !peer->read_wait;
    if (peer->read_wait || peer->read_used == sizeof(incoming)) return ML_DERP_STREAM_AGAIN;
    /* Header and body bytes may each arrive in separate TLS records. */
    assert(len);
    *data = incoming[peer->read_used++];
    return 1;
}

static int peer_write(void *context, uint8_t *data, size_t len) {
    peer_t *peer = context;
    if (peer->retry_buffer) {
        assert(peer->retry_buffer == data);
        assert(peer->retry_len == len);
    }
    if (peer->now < 1500) {
        peer->retry_buffer = data;
        peer->retry_len = len;
        return ML_DERP_STREAM_AGAIN;
    }
    peer->retry_buffer = NULL;
    size_t n = len > 17 ? 17 : len;
    assert(peer->output_used + n <= sizeof(peer->output));
    memcpy(peer->output + peer->output_used, data, n);
    peer->output_used += n;
    return (int)n;
}

static void test_bidirectional_backpressure(void) {
    ml_derp_stream_t stream = {0};
    peer_t peer = {0};
    uint8_t key[32], packet[1392];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(i + 41);
    for (size_t i = 0; i < sizeof(packet); ++i) packet[i] = (uint8_t)(i * 73);
    assert(ml_derp_stream_begin(&stream, 4, key, packet, sizeof(packet), 0, malloc) == 1);
    unsigned received = 0;
    for (; peer.now < 3000; peer.now += 10) {
        assert(ml_derp_stream_write(&stream, peer_write, &peer, peer.now) >= 0);
        ml_derp_stream_frame_t frame;
        int result = ml_derp_stream_read(&stream, peer_read, &peer, peer.now, malloc, &frame);
        assert(result >= 0);
        if (result) {
            if (received == 0) {
                assert(frame.type == 0x12 && frame.len == 8);
                assert(memcmp(frame.data, "relaypin", 8) == 0);
                /* Incoming HID/control must be serviced while TX is blocked.
                 * A PONG cannot replace the TLS record awaiting a retry. */
                assert(peer.now < 1500 && peer.output_used == 0);
                assert(ml_derp_stream_begin(&stream, 0x13, NULL, frame.data,
                                           frame.len, peer.now, malloc) < 0);
            } else {
                assert(received == 1 && frame.type == 6 && frame.len == 0);
                assert(frame.data == NULL);
            }
            ++received;
            free(frame.data);
        }
    }
    assert(received == 2 && stream.tx_data == NULL);
    assert(peer.output_used == 5 + 32 + sizeof(packet));
    const uint8_t expected_header[] = {4, 0, 0, 5, 0x90}; /* 32 + 1392 = 1424 */
    assert(memcmp(peer.output, expected_header, sizeof(expected_header)) == 0);
    assert(memcmp(peer.output + 5, key, sizeof(key)) == 0);
    assert(memcmp(peer.output + 37, packet, sizeof(packet)) == 0);
    /* The pending response now follows the complete datagram, with no lost,
     * duplicated, or reordered bytes after short writes and repeated retries. */
    assert(ml_derp_stream_begin(&stream, 0x13, NULL, incoming + 5, 8, peer.now, malloc) == 1);
    assert(ml_derp_stream_write(&stream, peer_write, &peer, peer.now) == 1);
    const uint8_t pong[] = {0x13, 0, 0, 0, 8, 'r', 'e', 'l', 'a', 'y', 'p', 'i', 'n'};
    assert(memcmp(peer.output + 1429, pong, sizeof(pong)) == 0);
    ml_derp_stream_reset(&stream);
}

static int stalled(void *context, uint8_t *data, size_t len) {
    (void)context; (void)data; (void)len;
    return ML_DERP_STREAM_AGAIN;
}

static int closed(void *context, uint8_t *data, size_t len) {
    (void)context; (void)data; (void)len;
    return 0;
}

static int oversized(void *context, uint8_t *data, size_t len) {
    (void)context;
    const uint8_t header[] = {5, 0, 1, 0, 1};
    assert(len == sizeof(header));
    memcpy(data, header, sizeof(header));
    return sizeof(header);
}

static void *no_memory(size_t size) {
    (void)size;
    return NULL;
}

static void test_connection_retirement(void) {
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        ml_derp_stream_t stream = {0};
        ml_derp_stream_frame_t frame;
        peer_t peer = {0};
        assert(ml_derp_stream_begin(&stream, 6, NULL, NULL, 0, 0, no_memory) < 0);
        assert(ml_derp_stream_begin(&stream, 6, NULL, NULL, 0, 0, malloc) == 1);
        assert(ml_derp_stream_write(&stream, stalled, NULL, 1000) == 0);
        assert(ml_derp_stream_write(&stream, stalled, NULL, 10001) < 0);
        /* Disconnect must free both an in-flight write and a fragmented read. */
        for (unsigned i = 0; i < 12; ++i)
            assert(ml_derp_stream_read(&stream, peer_read, &peer, i * 10, malloc, &frame) == 0);
        assert(stream.rx_data != NULL);
        assert(ml_derp_stream_read(&stream, closed, NULL, 130, malloc, &frame) < 0);
        ml_derp_stream_reset(&stream);
        assert(stream.tx_data == NULL && stream.rx_data == NULL && stream.header_used == 0);
        assert(ml_derp_stream_read(&stream, oversized, NULL, 0, no_memory, &frame) < 0);
        ml_derp_stream_reset(&stream);
    }
}

static void test_batched_peer_frames(void) {
    ml_derp_stream_t stream = {0};
    peer_t peer = {0};
    uint8_t first_key[32], second_key[32], first[44], second[45];
    memset(first_key, 0x41, sizeof(first_key));
    memset(second_key, 0x52, sizeof(second_key));
    memset(first, 0x63, sizeof(first));
    memset(second, 0x74, sizeof(second));
    const ml_derp_stream_write_frame_t frames[] = {
        {4, first_key, first, sizeof(first)},
        {4, second_key, second, sizeof(second)},
        {0x13, NULL, incoming + 5, 8},
    };
    assert(ml_derp_stream_begin_batch(&stream, frames, 3, 0, malloc) == 1);
    for (; peer.now < 2000; peer.now += 10)
        assert(ml_derp_stream_write(&stream, peer_write, &peer, peer.now) >= 0);
    assert(stream.tx_data == NULL && stream.tx_frames == 3);
    /* Exact on-wire fixtures: each destination and DERP length must survive
     * coalescing, a long blocked send, and partial TLS writes. */
    uint8_t expected[176] = {4, 0, 0, 0, 76};
    memset(expected + 5, 0x41, 32);
    memset(expected + 37, 0x63, 44);
    memcpy(expected + 81, (uint8_t[]){4, 0, 0, 0, 77}, 5);
    memset(expected + 86, 0x52, 32);
    memset(expected + 118, 0x74, 45);
    memcpy(expected + 163, (uint8_t[]){0x13, 0, 0, 0, 8}, 5);
    memcpy(expected + 168, "relaypin", 8);
    assert(peer.output_used == sizeof(expected));
    assert(memcmp(peer.output, expected, sizeof(expected)) == 0);
    ml_derp_stream_reset(&stream);
}

int main(void) {
    test_bidirectional_backpressure();
    test_connection_retirement();
    test_batched_peer_frames();
    puts("PASS DERP fragmented duplex stream and TLS backpressure");
}
