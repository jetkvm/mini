#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "peer_sctp_hid_internal.h"

static unsigned allocations, drains, updates;
static int lock_depth[2];
int media_lib_mutex_lock(void *lock, uint32_t timeout)
{
    (void)timeout;
    unsigned i = (uintptr_t)lock - 1;
    assert(i < 2 && lock_depth[i]++ == 0);
    return 0;
}
int media_lib_mutex_unlock(void *lock)
{
    unsigned i = (uintptr_t)lock - 1;
    assert(i < 2 && --lock_depth[i] == 0);
    return 0;
}
peer_sctp_frame_t *jetkvm_vendor_get_frame(peer_sctp_t *sctp, peer_sctp_stream_t *stream, const void *chunk)
{
    (void)sctp;
    uint16_t sequence;
    memcpy(&sequence, (const uint8_t *)chunk + 10, 2);
    peer_sctp_frame_t *frame = calloc(1, sizeof(*frame));
    assert(frame);
    frame->sequence = sequence;
    allocations++;
    /* Deliberately model the vendor's incorrect placement at rollover. */
    frame->next = stream->frames;
    stream->frames = frame;
    return frame;
}
void media_lib_free(void *pointer)
{
    free(pointer);
}
int buf_mngr_abandon(peer_sctp_buffers_t *buffers, uint32_t previous, uint32_t advanced)
{
    assert(lock_depth[0] == 1 && lock_depth[1] == 0);
    for (peer_sctp_buffer_t *b = buffers->head; b; b = b->next) {
        if (!(b->flags & 2) && b->tsn - previous <= advanced - previous) b->flags |= 4;
    }
    return 0;
}
void update_receive_ack_tsn(peer_sctp_t *sctp, bool sack, int line)
{
    (void)sctp; (void)line;
    assert(sack && lock_depth[0] == 0 && lock_depth[1] == 0);
    updates++;
    /* The forward TSN itself reaches the last message: no further advance. */
}
void check_send_all_stream(peer_sctp_t *sctp, bool send)
{
    (void)sctp;
    assert(send && !lock_depth[0] && !lock_depth[1]);
    drains++;
}
static void test_sequence_rollover(void)
{
    peer_sctp_t sctp = {.lock = (void *)1};
    peer_sctp_stream_t *stream = &sctp.streams[0];
    uint8_t chunk[16] = {0};
    const uint16_t sequence[] = {65535, 0, 65534, 1};
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(chunk + 10, &sequence[i], 2);
        peer_sctp_frame_t *frame = jetkvm_sctp_get_frame(&sctp, stream, chunk);
        /* Later fragment must find the same descriptor, including SSN 0. */
        assert(jetkvm_sctp_get_frame(&sctp, stream, chunk) == frame);
        assert(allocations == i + 1);
    }
    const uint16_t expected[] = {65534, 65535, 0, 1};
    peer_sctp_frame_t *frame = stream->frames;
    for (unsigned i = 0; i < 4; ++i) {
        assert(frame && frame->sequence == expected[i]);
        peer_sctp_frame_t *next = frame->next;
        free(frame); frame = next;
    }
    assert(!frame && stream->first_sequence == 65534 && stream->last_sequence == 1);
}
static void test_forward(uint32_t previous, uint32_t advanced)
{
    peer_sctp_frame_t complete = {.flags = 3, .fill_count = 1, .min_tsn = advanced, .max_tsn = advanced};
    peer_sctp_frame_t fragment = {.flags = 1, .fill_count = 1, .min_tsn = advanced - 1, .max_tsn = advanced - 1};
    complete.next = &fragment;
    /* Reliable RPC BEGIN is gap-ACKed while its END is still in flight. The
     * pinned vendor sets 0x02 on every reliable receive buffer at insertion. */
    peer_sctp_frame_t rpc_begin = {.flags = 2, .fill_count = 1, .min_tsn = advanced, .max_tsn = advanced};
    peer_sctp_buffer_t reliable = {.data = "rpc-begin", .size = 9, .flags = 3, .tsn = advanced, .stream_id = 0};
    peer_sctp_buffer_t partial = {.data = "x", .size = 1, .flags = 1, .tsn = advanced - 1, .stream_id = 4};
    partial.next = &reliable;
    peer_sctp_buffer_t received = {.data = "move", .size = 4, .flags = 1, .tsn = advanced, .stream_id = 4, .next = &partial};
    peer_sctp_buffers_t buffers = {.lock = (void *)2, .head = &received};
    peer_sctp_t sctp = {.lock = (void *)1, .receive_buffers = &buffers, .acknowledged_tsn = previous};
    sctp.streams[2] = (peer_sctp_stream_t){.flags = 1, .id = 4, .frames = &complete};
    sctp.streams[0] = (peer_sctp_stream_t){.flags = 0x21, .id = 0, .frames = &rpc_begin};
    uint8_t forward[] = {192, 0, 0, 12, advanced >> 24, advanced >> 16, advanced >> 8, advanced, 0, 4, 0, 1};
    unsigned before = drains;
    jetkvm_sctp_handle_forward_tsn(&sctp, forward);
    assert(sctp.acknowledged_tsn == advanced);
    assert(received.flags == 1); /* Complete received move survives abandonment. */
    assert(partial.flags == 5); /* Incomplete fragment does not masquerade as a move. */
    assert(reliable.flags == 3 && reliable.size == 9 && !strcmp(reliable.data, "rpc-begin"));
    assert(sctp.streams[0].frames == &rpc_begin && rpc_begin.flags == 2);
    assert(drains == before + 1 && updates == drains);
    jetkvm_sctp_handle_forward_tsn(&sctp, forward);
    assert(drains == before + 1); /* Duplicate / old FORWARD-TSN is inert. */
    forward[3] = 7;
    jetkvm_sctp_handle_forward_tsn(&sctp, forward);
    assert(drains == before + 1);
}
/* FORWARD-TSN to 103 with 100 and 102 received: the vendor's empty buffers
 * for 101 and 103 take the first two free buffers, which must have no block
 * that their removal would subtract from the bytes held. */
static void test_forward_skipped_buffers_hold_no_block(void)
{
    peer_sctp_buffer_t free_buffers[3];
    for (unsigned i = 0; i < 3; ++i) {
        free_buffers[i] = (peer_sctp_buffer_t){.data = malloc(1156), .allocated_size = 1156,
                                               .next = i < 2 ? &free_buffers[i + 1] : NULL};
    }
    peer_sctp_buffer_t later = {.data = "b", .size = 1, .flags = 1, .tsn = 102, .stream_id = 4};
    peer_sctp_buffer_t first = {.data = "a", .size = 1, .flags = 1, .tsn = 100, .stream_id = 4, .next = &later};
    peer_sctp_buffers_t buffers = {.lock = (void *)2, .head = &first, .free_list = free_buffers};
    peer_sctp_t sctp = {.lock = (void *)1, .receive_buffers = &buffers, .acknowledged_tsn = 99};
    const uint8_t forward[] = {192, 0, 0, 8, 0, 0, 0, 103};
    jetkvm_sctp_handle_forward_tsn(&sctp, forward);
    assert(sctp.acknowledged_tsn == 103);
    assert(!free_buffers[0].data && !free_buffers[0].allocated_size);
    assert(!free_buffers[1].data && !free_buffers[1].allocated_size);
    assert(free_buffers[2].data && free_buffers[2].allocated_size == 1156);
    free(free_buffers[2].data);
}
int main(void)
{
    test_forward_skipped_buffers_hold_no_block();
    test_sequence_rollover();
    test_forward(99, 102);
    test_forward(UINT32_MAX - 2, 0);
    puts("SCTP HID rollover, fragmentation and loss recovery: OK");
}
