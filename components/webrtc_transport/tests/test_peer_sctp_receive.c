/* peer_sctp_receive.c against a model of the vendor's receive cache and of
 * Chrome's sender (dcsctp). The cache refuses a chunk when the bytes held
 * plus the chunk reach its size, frees a message only once every chunk up to
 * its last is cumulatively acknowledged, and SACKs report the chunks held
 * above that. The sender sends new chunks within the advertised window,
 * retransmits a lost chunk outside it, and never sends a chunk again once a
 * SACK reported it held. Some chunks and their first retransmission are lost:
 * then new chunks fill the space the retransmission needs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "peer_sctp_hid_internal.h"

#define LIMIT (256U * 1024U)
#define CHUNK 1156U
#define MAX_CHUNKS 4096U
#define MAX_ROUNDS 2000U
/* Close to rollover, so every TSN comparison crosses it. */
#define FIRST_TSN UINT32_C(0xfffffe00)

int media_lib_mutex_lock(void *lock, uint32_t timeout)
{
    (void)lock; (void)timeout;
    return 0;
}
int media_lib_mutex_unlock(void *lock)
{
    (void)lock;
    return 0;
}
void *media_lib_realloc(void *pointer, size_t size)
{
    if (size == 0) {
        free(pointer);
        return NULL;
    }
    return realloc(pointer, size);
}

typedef struct {
    uint32_t size, last; /* the index of its message's last chunk */
    bool held, delivered, acked, gap_acked;
    unsigned transmissions;
} chunk_t;

static chunk_t chunks[MAX_CHUNKS];
static uint32_t chunk_count, vendor_calls, refusals;
/* Chunks that lose their first four transmissions together. */
static uint32_t burst_first, burst_count;
static peer_sctp_buffers_t buffers;
static peer_sctp_t sctp;

static uint32_t index_of(uint32_t tsn) { return tsn - FIRST_TSN; }

static bool cumulatively_acked(uint32_t i)
{
    return !peer_sctp_after(FIRST_TSN + i, sctp.acknowledged_tsn);
}

static void deliver(void)
{
    for (uint32_t i = 0; i < chunk_count; ++i) {
        chunk_t *c = &chunks[i];
        if (c->held && !c->delivered && cumulatively_acked(c->last)) {
            c->delivered = true;
            buffers.fill_size -= c->size;
        }
    }
}

int jetkvm_vendor_packet_hdlr(peer_sctp_t *s, const void *wire, int control)
{
    assert(s == &sctp && !control);
    const uint8_t *w = wire;
    const uint32_t tsn = (uint32_t)w[4] << 24 | (uint32_t)w[5] << 16 |
                         (uint32_t)w[6] << 8 | w[7];
    const uint32_t size = ((uint32_t)w[2] << 8 | w[3]) - 16U;
    chunk_t *c = &chunks[index_of(tsn)];
    assert(size == c->size);
    vendor_calls++;
    sctp.sack_count = 0;
    if (!peer_sctp_after(tsn, sctp.acknowledged_tsn) || c->held) return 0;
    if (buffers.fill_size + size >= buffers.limit_size) {
        refusals++;
        return -1;
    }
    c->held = true;
    buffers.fill_size += size;
    while (index_of(sctp.acknowledged_tsn + 1U) < chunk_count &&
           chunks[index_of(sctp.acknowledged_tsn + 1U)].held) {
        sctp.acknowledged_tsn++;
    }
    deliver();
    return 0;
}

static void add_message(uint32_t size)
{
    const uint32_t first = chunk_count;
    for (uint32_t left = size; left; ) {
        const uint32_t part = left < CHUNK ? left : CHUNK;
        assert(chunk_count < MAX_CHUNKS);
        chunks[chunk_count++].size = part;
        left -= part;
    }
    for (uint32_t i = first; i < chunk_count; ++i) chunks[i].last = chunk_count - 1U;
}

static void reset(void)
{
    memset(chunks, 0, sizeof(chunks));
    memset(&sctp, 0, sizeof(sctp));
    memset(&buffers, 0, sizeof(buffers));
    chunk_count = vendor_calls = refusals = burst_count = 0;
    buffers.limit_size = LIMIT;
    sctp.receive_buffers = &buffers;
    sctp.acknowledged_tsn = FIRST_TSN - 1U;
}

/* One transmission to the Mini: indices 13, 110, 207 ... lose their first
 * two transmissions, the burst its first four. */
static void transmit(uint32_t i)
{
    uint8_t wire[16] = {0};
    const uint32_t length = 16U + chunks[i].size, tsn = FIRST_TSN + i;
    wire[2] = (uint8_t)(length >> 8);
    wire[3] = (uint8_t)length;
    wire[4] = (uint8_t)(tsn >> 24);
    wire[5] = (uint8_t)(tsn >> 16);
    wire[6] = (uint8_t)(tsn >> 8);
    wire[7] = (uint8_t)tsn;
    const unsigned losses = i - burst_first < burst_count ? 4U : i % 97U == 13U ? 2U : 0U;
    if (chunks[i].transmissions++ < losses) return;
    sctp.sack_count = 5;
    const int result = jetkvm_sctp_receive_data(&sctp, wire, 0);
    assert(result == 0 || (result < 0 && !chunks[i].held));
    /* Every DATA chunk, also a refused one, keeps SACKs going. */
    assert(sctp.sack_count == 0);
}

/* Round trips until every message was delivered: the retransmissions, new
 * chunks within the window, then the SACK. */
static unsigned run(void)
{
    uint32_t window = LIMIT, next = 0;
    for (unsigned round = 1; round <= MAX_ROUNDS; ++round) {
        for (uint32_t i = 0; i < next; ++i) {
            if (!chunks[i].acked && !chunks[i].gap_acked) transmit(i);
        }
        /* With nothing outstanding, one chunk goes out whatever the window. */
        bool probe = true;
        for (uint32_t i = 0; i < next; ++i) probe &= chunks[i].acked || chunks[i].gap_acked;
        uint32_t budget = window;
        while (next < chunk_count && (chunks[next].size <= budget || probe)) {
            budget -= chunks[next].size <= budget ? chunks[next].size : budget;
            probe = false;
            transmit(next++);
        }
        for (uint32_t i = 0; i < next; ++i) {
            chunks[i].acked = cumulatively_acked(i);
            /* Reneging would stall dcsctp: a gap-acknowledged chunk stays held. */
            assert(!chunks[i].gap_acked || chunks[i].acked || chunks[i].held);
            chunks[i].gap_acked = !chunks[i].acked && chunks[i].held;
        }
        window = jetkvm_sctp_receive_window(&buffers);
        bool done = true;
        for (uint32_t i = 0; i < chunk_count; ++i) done &= chunks[i].delivered;
        if (done) return round;
    }
    return 0;
}

/* 32 KiB file-drop messages with small RPC requests between them, and one
 * OS Services clipboard request of the largest size the Mini accepts, most
 * of which is lost at first: once the chunks behind it filled the window, it
 * can complete only from the reserve. */
static void test_drop_with_loss_completes(void)
{
    reset();
    for (unsigned i = 0; i < 48; ++i) {
        add_message(32U * 1024U + 64U);
        if (i % 4U == 3U) add_message(200U);
        if (i == 20) {
            burst_first = chunk_count + 2U;
            add_message(160U * 1024U - 1U);
            burst_count = chunk_count - burst_first - 1U;
        }
    }
    const unsigned rounds = run();
    printf("%u chunks delivered in %u rounds, %u refused by the cache\n",
           chunk_count, rounds, refusals);
    assert(rounds);
    assert(buffers.fill_size == 0);
}

/* A chunk past the next one is dropped before the vendor once the window is
 * full; the next one still reaches it; the window never wraps. */
static void test_window_and_admission(void)
{
    reset();
    add_message(CHUNK);
    add_message(CHUNK);
    buffers.fill_size = LIMIT - PEER_SCTP_RECEIVE_RESERVE - CHUNK + 1U;
    transmit(1);
    assert(vendor_calls == 0 && !chunks[1].held);
    transmit(0);
    assert(vendor_calls == 1 && chunks[0].delivered);
    buffers.fill_size = LIMIT - PEER_SCTP_RECEIVE_RESERVE - 10U;
    assert(jetkvm_sctp_receive_window(&buffers) == 10U);
    buffers.fill_size = LIMIT + 100U;
    assert(jetkvm_sctp_receive_window(&buffers) == 0U);
}

/* The free buffer the vendor takes next counts at the chunk's size; a
 * malformed chunk shorter than its header never reaches the vendor. */
static void test_reused_buffer_counts_its_chunk(void)
{
    reset();
    add_message(200U);
    peer_sctp_buffer_t free_buffer = {.data = malloc(CHUNK), .allocated_size = CHUNK};
    buffers.free_list = &free_buffer;
    transmit(0);
    assert(vendor_calls == 1 && free_buffer.allocated_size == 200U && free_buffer.data);
    const uint8_t short_chunk[16] = {0, 0, 0, 15, 0xff, 0xff, 0xfe, 0x01};
    assert(jetkvm_sctp_receive_data(&sctp, short_chunk, 0) < 0 && vendor_calls == 1);
    free(free_buffer.data);
}

int main(void)
{
    test_reused_buffer_counts_its_chunk();
    test_window_and_admission();
    test_drop_with_loss_completes();
    puts("peer_sctp_receive: ok");
    return 0;
}
