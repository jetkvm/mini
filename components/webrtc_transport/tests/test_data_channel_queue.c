#include "data_channel_queue.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A send buffer of `room` bytes that takes a message only when it fits, as
 * esp_peer does, and records what the browser gets. */
typedef struct {
    size_t room;
    int fail_byte; /* A message starting with this byte fails; -1 none. */
    char got[64][16];
    uint16_t got_stream[64];
    size_t got_count;
    size_t calls;
    uint16_t lost_stream;
    uint32_t lost_number;
    size_t lost_count;
} peer_t;

static data_channel_send_result_t peer_send(void *ctx, uint16_t stream_id,
                                            bool text, const uint8_t *data,
                                            size_t size)
{
    peer_t *peer = ctx;
    peer->calls++;
    assert(text == (stream_id == 1U));
    if ((int)data[0] == peer->fail_byte) {
        return DATA_CHANNEL_FAILED;
    }
    if (size > peer->room) {
        return DATA_CHANNEL_BLOCKED;
    }
    peer->room -= size;
    snprintf(peer->got[peer->got_count], sizeof(peer->got[0]), "%.*s",
             (int)size, (const char *)data);
    peer->got_stream[peer->got_count++] = stream_id;
    return DATA_CHANNEL_SENT;
}

static data_channel_send_result_t send_text(data_channel_queue_t *queue,
                                            peer_t *peer, uint16_t stream,
                                            const char *text)
{
    return data_channel_queue_send(queue, peer_send, peer, stream,
                                   stream == 1U, (const uint8_t *)text,
                                   strlen(text));
}

static void peer_lost(void *ctx, uint16_t stream_id, uint32_t number)
{
    peer_t *peer = ctx;
    peer->lost_stream = stream_id;
    peer->lost_number = number;
    peer->lost_count++;
}

static peer_t new_peer(size_t room)
{
    return (peer_t){.room = room, .fail_byte = -1};
}

static void sends_at_once_when_the_buffer_takes_it(void)
{
    data_channel_queue_t queue = {0};
    peer_t peer = new_peer(100);
    assert(send_text(&queue, &peer, 1, "answer") == DATA_CHANNEL_SENT);
    assert(peer.got_count == 1 && strcmp(peer.got[0], "answer") == 0);
    assert(queue.head == NULL && queue.bytes == 0);
}

static void a_refused_message_waits_and_goes_when_the_buffer_drains(void)
{
    data_channel_queue_t queue = {0};
    peer_t peer = new_peer(4);
    assert(send_text(&queue, &peer, 1, "answer") == DATA_CHANNEL_QUEUED);
    const size_t held = queue.bytes;
    assert(peer.got_count == 0 && held > 6);
    /* Still full: nothing goes, nothing is lost. */
    assert(data_channel_queue_drain(&queue, peer_send, NULL, &peer) == 0);
    assert(peer.got_count == 0 && queue.bytes == held);
    peer.room = 100;
    assert(data_channel_queue_drain(&queue, peer_send, NULL, &peer) == 0);
    assert(peer.got_count == 1 && strcmp(peer.got[0], "answer") == 0);
    assert(queue.head == NULL && queue.tail == NULL && queue.bytes == 0);
}

static void a_stream_keeps_its_order(void)
{
    data_channel_queue_t queue = {0};
    peer_t peer = new_peer(5);
    assert(send_text(&queue, &peer, 1, "first-long") == DATA_CHANNEL_QUEUED);
    /* It would fit, but goes after the first. */
    assert(send_text(&queue, &peer, 1, "2nd") == DATA_CHANNEL_QUEUED);
    assert(peer.got_count == 0 && peer.calls == 1);
    peer.room = 12;
    data_channel_queue_drain(&queue, peer_send, NULL, &peer);
    /* The first went; the second waits for room, still in order. */
    assert(peer.got_count == 1 && strcmp(peer.got[0], "first-long") == 0);
    peer.room = 3;
    data_channel_queue_drain(&queue, peer_send, NULL, &peer);
    assert(peer.got_count == 2 && strcmp(peer.got[1], "2nd") == 0);
    assert(queue.head == NULL && queue.bytes == 0);
}

static void streams_do_not_wait_for_each_other(void)
{
    data_channel_queue_t queue = {0};
    peer_t peer = new_peer(4);
    assert(send_text(&queue, &peer, 1, "large answer") == DATA_CHANNEL_QUEUED);
    /* A HIDRPC message goes at once past the waiting RPC answer. */
    assert(send_text(&queue, &peer, 2, "k1") == DATA_CHANNEL_SENT);
    assert(send_text(&queue, &peer, 2, "k2xx") == DATA_CHANNEL_QUEUED);
    assert(send_text(&queue, &peer, 2, "k3") == DATA_CHANNEL_QUEUED);
    peer.room = 6;
    data_channel_queue_drain(&queue, peer_send, NULL, &peer);
    /* The answer still does not fit; HIDRPC drains behind it, in order. */
    assert(peer.got_count == 3);
    assert(strcmp(peer.got[1], "k2xx") == 0 && strcmp(peer.got[2], "k3") == 0);
    assert(queue.head != NULL && queue.head == queue.tail);
    peer.room = 12;
    data_channel_queue_drain(&queue, peer_send, NULL, &peer);
    assert(peer.got_count == 4 && strcmp(peer.got[3], "large answer") == 0);
    assert(queue.head == NULL && queue.tail == NULL);
}

static void a_failure_is_reported_and_drops_only_that_message(void)
{
    data_channel_queue_t queue = {0};
    peer_t peer = new_peer(100);
    peer.fail_byte = 'x';
    assert(send_text(&queue, &peer, 1, "xlost") == DATA_CHANNEL_FAILED);
    assert(queue.head == NULL);

    peer.room = 0;
    assert(send_text(&queue, &peer, 1, "a") == DATA_CHANNEL_QUEUED);
    assert(send_text(&queue, &peer, 1, "xb") == DATA_CHANNEL_QUEUED);
    assert(send_text(&queue, &peer, 1, "c") == DATA_CHANNEL_QUEUED);
    const uint32_t lost_number = queue.queued - 1U;
    peer.room = 100;
    assert(data_channel_queue_drain(&queue, peer_send, peer_lost, &peer) ==
           1);
    assert(peer.lost_count == 1 && peer.lost_stream == 1 &&
           peer.lost_number == lost_number);
    assert(peer.got_count == 2);
    assert(strcmp(peer.got[0], "a") == 0 && strcmp(peer.got[1], "c") == 0);
    assert(queue.head == NULL && queue.bytes == 0);
}

/* The limit counts each message's header too: small messages cannot hold
 * more memory than it allows. */
static void the_queue_holds_at_most_its_bytes(void)
{
    static uint8_t big[DATA_CHANNEL_QUEUE_MAX_BYTES];
    memset(big, 'b', sizeof(big));
    data_channel_queue_t queue = {0};
    peer_t peer = new_peer(0);
    assert(data_channel_queue_send(&queue, peer_send, &peer, 2, false, big,
                                   sizeof(big)) == DATA_CHANNEL_FAILED);
    assert(data_channel_queue_send(&queue, peer_send, &peer, 2, false, big,
                                   sizeof(big) - 100) == DATA_CHANNEL_QUEUED);
    unsigned taken = 0;
    while (send_text(&queue, &peer, 2, "z") == DATA_CHANNEL_QUEUED) taken++;
    assert(taken < 100 && queue.bytes <= DATA_CHANNEL_QUEUE_MAX_BYTES);
    data_channel_queue_clear(&queue);
    assert(queue.head == NULL && queue.tail == NULL && queue.bytes == 0);
}

static void a_closed_stream_drops_only_its_messages(void)
{
    data_channel_queue_t queue = {0};
    peer_t peer = new_peer(0);
    assert(send_text(&queue, &peer, 1, "r1") == DATA_CHANNEL_QUEUED);
    assert(send_text(&queue, &peer, 2, "h1") == DATA_CHANNEL_QUEUED);
    assert(send_text(&queue, &peer, 1, "r2") == DATA_CHANNEL_QUEUED);
    data_channel_queue_drop_stream(&queue, 1);
    assert(queue.head != NULL && queue.head == queue.tail);
    /* The tail is right: a later message queues behind the survivor. */
    assert(send_text(&queue, &peer, 2, "h2") == DATA_CHANNEL_QUEUED);
    peer.room = 100;
    data_channel_queue_drain(&queue, peer_send, NULL, &peer);
    assert(peer.got_count == 2);
    assert(strcmp(peer.got[0], "h1") == 0 && strcmp(peer.got[1], "h2") == 0);
    assert(peer.got_stream[0] == 2 && peer.got_stream[1] == 2);
}

/* The reboot waits for its response and willReboot only: not for later
 * messages on their stream, nor for messages on other streams. */
static void holds_sees_only_the_messages_up_to_a_number(void)
{
    data_channel_queue_t queue = {0};
    peer_t peer = new_peer(0);
    assert(send_text(&queue, &peer, 2, "hidrpc-larger-than-16") ==
           DATA_CHANNEL_QUEUED);
    assert(send_text(&queue, &peer, 1, "answer") == DATA_CHANNEL_QUEUED);
    assert(send_text(&queue, &peer, 1, "willReboot") == DATA_CHANNEL_QUEUED);
    const uint32_t last = queue.queued;
    assert(send_text(&queue, &peer, 1, "later-long") == DATA_CHANNEL_QUEUED);
    assert(data_channel_queue_holds(&queue, 1, last));
    peer.room = 16;
    data_channel_queue_drain(&queue, peer_send, NULL, &peer);
    /* The answer and willReboot went; the later answer and the HIDRPC
     * message (too large now) still wait. */
    assert(peer.got_count == 2 && strcmp(peer.got[1], "willReboot") == 0);
    assert(!data_channel_queue_holds(&queue, 1, last));
    assert(data_channel_queue_holds(&queue, 1, queue.queued));
    assert(data_channel_queue_holds(&queue, 2, last));
    data_channel_queue_clear(&queue);
    assert(!data_channel_queue_holds(&queue, 1, queue.queued));
}

int main(void)
{
    sends_at_once_when_the_buffer_takes_it();
    a_refused_message_waits_and_goes_when_the_buffer_drains();
    a_stream_keeps_its_order();
    streams_do_not_wait_for_each_other();
    a_failure_is_reported_and_drops_only_that_message();
    the_queue_holds_at_most_its_bytes();
    a_closed_stream_drops_only_its_messages();
    holds_sees_only_the_messages_up_to_a_number();
    puts("data channel queue: PASS");
    return 0;
}
