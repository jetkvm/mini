/* Host test for peer_sctp_reconfig.c against a model of the vendor calls. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "peer_sctp_hid_internal.h"
#include "peer_sctp_streams.h"

#define SEND_LOCK ((void *)3)

static int send_lock_depth;
static uint8_t written[16][48];
static int written_length[16];
static unsigned writes;
static uint16_t closed_ids[16];
static unsigned closes;
static int64_t now_us;
static unsigned sack_checks;

int64_t esp_timer_get_time(void)
{
    return now_us;
}

static void at_ms(uint32_t ms)
{
    now_us = (int64_t)ms * 1000;
}

int jetkvm_vendor_check_sack(peer_sctp_t *sctp)
{
    (void)sctp;
    sack_checks++;
    return 7;
}

int media_lib_mutex_lock(void *lock, uint32_t timeout)
{
    (void)timeout;
    assert(lock == SEND_LOCK && send_lock_depth++ == 0);
    return 0;
}

int media_lib_mutex_unlock(void *lock)
{
    assert(lock == SEND_LOCK && --send_lock_depth == 0);
    return 0;
}

int jetkvm_vendor_fill_header_and_write(peer_sctp_t *sctp, void *packet,
                                        int length)
{
    assert(send_lock_depth == 1);
    assert(packet == (uint8_t *)sctp + 100);
    assert(length > 12 && length % 4 == 0 && length - 12 <= 48 && writes < 16);
    memcpy(written[writes], (uint8_t *)packet + 12, (size_t)length - 12U);
    written_length[writes++] = length - 12;
    return 0;
}

/* The vendor frees a slot once both directions are reset. Its release_stream
 * frees the label and the frames and clears flag bits 0-4; the outgoing SSN
 * (send_count), the last delivered SSN and flag bits 5-7 stay. */
int jetkvm_vendor_stream_closed(peer_sctp_t *sctp, peer_sctp_stream_t *stream)
{
    (void)sctp;
    if (!(stream->flags & PEER_SCTP_STREAM_CLOSE_REPORTED)) {
        stream->flags |= PEER_SCTP_STREAM_CLOSE_REPORTED;
        assert(closes < 16);
        closed_ids[closes++] = stream->id;
    }
    const uint8_t both = PEER_SCTP_STREAM_LOCAL_RESET |
                         PEER_SCTP_STREAM_REMOTE_RESET;
    if ((stream->flags & both) == both) {
        stream->label = NULL;
        stream->frames = stream->tail = NULL;
        stream->first_sequence = stream->last_sequence = 0;
        stream->flags &= 0xe0U;
    }
    return 0;
}

/* Mirrors jetkvm_vendor_send_dcep_ack() in
 * third_party/esp_peer/src/core/sctp.c: the ACK takes a TSN only when the
 * send buffer takes it, and an SSN in any case. */
static int send_buffer_full;

void jetkvm_vendor_send_dcep_ack(peer_sctp_t *sctp, uint16_t id)
{
    peer_sctp_stream_t *stream = jetkvm_sctp_get_stream(sctp, id);
    if (!stream) return;
    if (!send_buffer_full) sctp->send_tsn++;
    stream->send_count++;
}

static char rpc[] = "rpc";
static char terminal_a[] = "guest-terminal";
static char terminal_b[] = "guest-terminal";
static char terminal_c[] = "guest-terminal";

static peer_sctp_t *association(void)
{
    peer_sctp_t *sctp = calloc(1, sizeof(*sctp));
    assert(sctp);
    sctp->send_lock = SEND_LOCK;
    /* Every DATA chunk the browser sent before its requests arrived. */
    sctp->acknowledged_tsn = 77;
    /* Ours: the next request and DATA chunk, both across rollover, and the
     * browser has every DATA chunk we sent. */
    sctp->request_sequence = UINT32_C(0xfffffffe);
    sctp->send_tsn = 300;
    sctp->send_acked_tsn = 299;
    sctp->streams[0] = (peer_sctp_stream_t){.label = rpc, .id = 0, .flags = 3};
    sctp->streams[3] = (peer_sctp_stream_t){.label = terminal_a, .id = 2, .flags = 3};
    sctp->streams[5] = (peer_sctp_stream_t){.label = terminal_b, .id = 4, .flags = 3};
    sctp->streams[9] = (peer_sctp_stream_t){.label = terminal_c, .id = 6, .flags = 3};
    writes = closes = sack_checks = 0;
    at_ms(1000);
    return sctp;
}

static size_t put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
    return 2;
}

static size_t put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
    return 4;
}

/* Appends an Outgoing SSN Reset Request. */
static size_t request(uint8_t *p, uint32_t sequence, const uint16_t *ids,
                      size_t count)
{
    size_t n = put16(p, 13);
    n += put16(p + n, (uint16_t)(16U + 2U * count));
    n += put32(p + n, sequence);
    n += put32(p + n, 0);
    n += put32(p + n, 77);
    for (size_t i = 0; i < count; ++i) n += put16(p + n, ids[i]);
    while (n % 4U) p[n++] = 0;
    return n;
}

static size_t response(uint8_t *p, uint32_t sequence, uint32_t result)
{
    size_t n = put16(p, 16);
    n += put16(p + n, 12);
    n += put32(p + n, sequence);
    n += put32(p + n, result);
    return n;
}

static void chunk_header(uint8_t *chunk, size_t length)
{
    chunk[0] = 130;
    chunk[1] = 0;
    put16(chunk + 2, (uint16_t)length);
}

static void expect_result(unsigned index, uint32_t sequence, uint32_t result)
{
    static const uint8_t head[8] = {130, 0, 0, 16, 0, 16, 0, 12};
    assert(index < writes && written_length[index] == 16);
    assert(memcmp(written[index], head, 8) == 0);
    uint8_t tail[8];
    put32(tail, sequence);
    put32(tail + 4, result);
    assert(memcmp(written[index] + 8, tail, 8) == 0);
}

static void expect_response(unsigned index, uint32_t sequence)
{
    expect_result(index, sequence, 1);
}

/* Our Outgoing SSN Reset Request of the listed streams. */
static void expect_request(unsigned index, uint32_t sequence, uint32_t last_tsn,
                           const uint16_t *ids, size_t count)
{
    uint8_t expected[48] = {130, 0};
    put16(expected + 2, (uint16_t)(4U + 16U + 2U * count));
    size_t n = 4 + put16(expected + 4, 13);
    n += put16(expected + n, (uint16_t)(16U + 2U * count));
    n += put32(expected + n, sequence);
    n += put32(expected + n, 0);
    n += put32(expected + n, last_tsn);
    for (size_t i = 0; i < count; ++i) n += put16(expected + n, ids[i]);
    while (n % 4U) expected[n++] = 0;
    assert(index < writes && written_length[index] == (int)n);
    assert(memcmp(written[index], expected, n) == 0);
}

/* The browser's RE-CONFIG chunk with one response. */
static void answer(peer_sctp_t *sctp, uint32_t sequence, uint32_t result)
{
    uint8_t chunk[16];
    size_t n = 4 + response(chunk + 4, sequence, result);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
}

static void test_labels(void)
{
    char label[PEER_SCTP_STREAM_LABEL_CAPACITY];
    uint16_t id = 0;
    const uint16_t ids[] = {0, 7, 10, 999, 65535};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        assert(peer_sctp_stream_label(label, ids[i]) == strlen(label));
        assert(peer_sctp_parse_stream_label(label, &id) && id == ids[i]);
    }
    assert(strcmp((peer_sctp_stream_label(label, 65535), label), "\x1f" "65535") == 0);
    const char *invalid[] = {"", "\x1f", "\x1f" "01", "\x1f" "65536",
                             "\x1f" "123456", "\x1f" "1a", "1", "rpc",
                             "guest-terminal"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!peer_sctp_parse_stream_label(invalid[i], &id));
    }
    assert(!peer_sctp_parse_stream_label(NULL, &id));
}

#define OURS UINT32_C(0xfffffffe)

/* The browser closes the second of three channels named alike: we answer,
 * reset that stream of ours alone and free the slot. */
static void test_browser_close(void)
{
    peer_sctp_t *sctp = association();
    uint8_t chunk[64];
    const uint16_t ids[] = {4};
    size_t n = 4 + request(chunk + 4, UINT32_C(0x9abcdef0), ids, 1);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_response(0, UINT32_C(0x9abcdef0));
    expect_request(1, OURS, 299, ids, 1);
    assert(writes == 2 && closes == 1 && closed_ids[0] == 4);
    assert(sctp->streams[5].flags == 0 && !sctp->streams[5].label);
    assert(sctp->streams[3].label == terminal_a && sctp->streams[3].flags == 3);
    assert(sctp->streams[9].label == terminal_c && sctp->streams[9].flags == 3);
    /* A retransmitted request is answered again; nothing else happens. */
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_response(2, UINT32_C(0x9abcdef0));
    assert(writes == 3 && closes == 1);
    assert(send_lock_depth == 0);
    free(sctp);
}

/* We close one channel by stream; the browser answers and resets its side,
 * in either order or in one chunk. A late answer must not close the channel
 * that reuses the slot. */
static void test_local_close(int order)
{
    peer_sctp_t *sctp = association();
    char label[PEER_SCTP_STREAM_LABEL_CAPACITY];
    peer_sctp_stream_label(label, 6);
    const uint16_t ids[] = {6};
    assert(jetkvm_sctp_send_recfg(sctp, label) == 0);
    expect_request(0, OURS, 299, ids, 1);
    assert(writes == 1 && closes == 1 && closed_ids[0] == 6);
    assert(sctp->streams[9].flags & PEER_SCTP_STREAM_LOCAL_RESET);
    assert(sctp->streams[3].label == terminal_a && sctp->streams[5].label == terminal_b);
    /* A second close sends nothing. */
    assert(jetkvm_sctp_send_recfg(sctp, label) == 0 && writes == 1);

    uint8_t chunk[96];
    size_t n = 4;
    if (order == 0) {
        n += response(chunk + n, OURS, 1);
        n += request(chunk + n, 41, ids, 1);
        chunk_header(chunk, n);
        assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    } else if (order == 1) {
        n += request(chunk + n, 41, ids, 1);
        n += response(chunk + n, OURS, 1);
        chunk_header(chunk, n);
        assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    } else {
        answer(sctp, OURS, 1);
        assert(sctp->streams[9].flags & PEER_SCTP_STREAM_OPEN);
        assert(writes == 1 && closes == 1);
        n = 4 + request(chunk + 4, 41, ids, 1);
        chunk_header(chunk, n);
        assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    }
    assert(writes == 2);
    expect_response(1, 41);
    assert(closes == 1);
    assert(sctp->streams[9].flags == 0);
    assert(sctp->streams[3].flags == 3 && sctp->streams[5].flags == 3);
    /* The browser opens a new channel in the slot; our old answer repeats. */
    static char reused[] = "guest-terminal";
    sctp->streams[9] = (peer_sctp_stream_t){.label = reused, .id = 6, .flags = 3};
    answer(sctp, OURS, 1);
    assert(sctp->streams[9].flags == 3 && writes == 2 && closes == 1);
    free(sctp);
}

static void test_many_streams(void)
{
    peer_sctp_t *sctp = association();
    uint8_t chunk[64];
    const uint16_t ids[] = {2, 9, 6};
    size_t n = 4 + request(chunk + 4, 5, ids, 3);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_response(0, 5);
    const uint16_t first[] = {2};
    expect_request(1, OURS, 299, first, 1);
    assert(writes == 2 && closes == 2 && closed_ids[0] == 2 && closed_ids[1] == 6);
    /* Stream 6 waits for our next request, and keeps its slot until then. */
    assert(sctp->streams[3].flags == 0);
    assert(sctp->streams[9].flags & PEER_SCTP_STREAM_REMOTE_RESET);
    assert(sctp->streams[5].label == terminal_b && sctp->streams[5].flags == 3);
    assert(sctp->streams[0].flags == 3);
    /* No stream listed: every stream. */
    n = 4 + request(chunk + 4, 6, NULL, 0);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_response(2, 6);
    assert(writes == 3);
    assert(closes == 4 && closed_ids[2] == 0 && closed_ids[3] == 4);
    /* Our first request is answered: one more resets the rest. */
    answer(sctp, OURS, 1);
    const uint16_t rest[] = {0, 4, 6};
    expect_request(3, OURS + 1, 299, rest, 3);
    assert(writes == 4 && closes == 4);
    assert(sctp->streams[0].flags == 0 && sctp->streams[5].flags == 0);
    assert(sctp->streams[9].flags == 0);
    free(sctp);
}

static void test_malformed(void)
{
    peer_sctp_t *sctp = association();
    uint8_t chunk[128] = {0};
    const uint16_t ids[] = {2};
    /* A parameter longer than its chunk. */
    size_t n = 4 + request(chunk + 4, 5, ids, 1);
    chunk_header(chunk, n - 4);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    /* A zero-length parameter stops the walk. */
    memset(chunk, 0, sizeof(chunk));
    n = 4 + put16(chunk + 4, 99) + put16(chunk + 6, 0);
    n += request(chunk + n, 5, ids, 1);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    /* A request too short for its fixed part, an Incoming SSN Reset Request
     * and oversized responses are ignored; the next parameter still counts. */
    n = 4 + put16(chunk + 4, 13) + put16(chunk + 6, 12);
    n += put32(chunk + n, 1) + put32(chunk + n + 4, 2);
    n += put16(chunk + n, 14) + put16(chunk + n + 2, 16);
    n += put32(chunk + n, 1) + put32(chunk + n + 4, 0) + put32(chunk + n + 8, 0);
    n += put16(chunk + n, 16) + put16(chunk + n + 2, 24);
    memset(chunk + n, 0, 20);
    n += 20;
    n += request(chunk + n, 8, ids, 1);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    assert(writes == 2);
    expect_response(0, 8);
    expect_request(1, OURS, 299, ids, 1);
    /* Only the stream labels this adapter makes, or a vendor label. */
    char label[PEER_SCTP_STREAM_LABEL_CAPACITY];
    peer_sctp_stream_label(label, 2);
    assert(jetkvm_sctp_send_recfg(sctp, label) == -1);
    peer_sctp_stream_label(label, 8);
    assert(jetkvm_sctp_send_recfg(sctp, label) == -1);
    assert(jetkvm_sctp_send_recfg(sctp, "missing") == -1);
    assert(jetkvm_sctp_send_recfg(sctp, NULL) == -1);
    assert(jetkvm_sctp_send_recfg(sctp, "guest-terminal") == 0);
    assert(jetkvm_sctp_send_recfg(sctp, "rpc") == 0);
    assert(writes == 2 && closes == 3 && closed_ids[1] == 4 && closed_ids[2] == 0);
    answer(sctp, OURS, 1);
    const uint16_t rest[] = {0, 4};
    expect_request(2, OURS + 1, 299, rest, 2);
    free(sctp);
}

/* The request overtakes the last DATA chunk sent before it: answer "in
 * progress" until that chunk arrives, then reset when the browser repeats
 * the request (RFC 6525 5.2.2). */
static void test_reset_before_last_data(void)
{
    peer_sctp_t *sctp = association();
    sctp->acknowledged_tsn = 76;
    uint8_t chunk[64];
    const uint16_t ids[] = {4};
    size_t n = 4 + request(chunk + 4, 41, ids, 1);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_result(0, 41, 6);
    assert(writes == 1 && closes == 0 && sctp->streams[5].flags == 3);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_result(1, 41, 6);
    assert(writes == 2 && closes == 0);
    sctp->acknowledged_tsn = 77;
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_result(2, 41, 1);
    expect_request(3, OURS, 299, ids, 1);
    assert(closes == 1 && sctp->streams[5].flags == 0);
    free(sctp);
}

/* A late copy of a request that was already performed is answered as before
 * and must not close the channel that reused the stream; an older request
 * is stale (RFC 6525 5.2.1). */
static void test_repeated_request_after_reuse(void)
{
    peer_sctp_t *sctp = association();
    uint8_t chunk[64], old[64];
    const uint16_t ids[] = {4};
    size_t n = 4 + request(chunk + 4, 41, ids, 1);
    chunk_header(chunk, n);
    size_t m = 4 + request(old + 4, 40, ids, 1);
    chunk_header(old, m);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_request(1, OURS, 299, ids, 1);
    assert(closes == 1 && sctp->streams[5].flags == 0);
    answer(sctp, OURS, 1);
    static char reused[] = "guest-terminal";
    sctp->streams[5] = (peer_sctp_stream_t){.label = reused, .id = 4, .flags = 3};
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_result(2, 41, 1);
    assert(writes == 3 && closes == 1 && sctp->streams[5].flags == 3);
    assert(jetkvm_sctp_handle_recfg(sctp, old) == 0);
    expect_result(3, 40, 5);
    assert(writes == 4 && closes == 1);
    assert(sctp->streams[5].label == reused && sctp->streams[5].flags == 3);
    /* The next request closes it. */
    n = 4 + request(chunk + 4, 42, ids, 1);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_result(4, 42, 1);
    expect_request(5, OURS + 1, 299, ids, 1);
    assert(closes == 2 && sctp->streams[5].flags == 0);
    free(sctp);
}

static void close_stream(peer_sctp_t *sctp, uint16_t id)
{
    char label[PEER_SCTP_STREAM_LABEL_CAPACITY];
    peer_sctp_stream_label(label, id);
    assert(jetkvm_sctp_send_recfg(sctp, label) == 0);
}

/* One request at a time (RFC 6525 5.1.1): the streams closed while ours is
 * in flight go out together once it is answered. */
static void test_one_request_in_flight(void)
{
    peer_sctp_t *sctp = association();
    close_stream(sctp, 2);
    close_stream(sctp, 4);
    close_stream(sctp, 6);
    const uint16_t first[] = {2}, rest[] = {4, 6};
    assert(writes == 1 && closes == 3);
    expect_request(0, OURS, 299, first, 1);
    assert(sctp->streams[3].flags & PEER_SCTP_STREAM_LOCAL_RESET);
    assert(!(sctp->streams[5].flags & PEER_SCTP_STREAM_LOCAL_RESET));
    sctp->send_tsn = 310;
    sctp->send_acked_tsn = 309;
    answer(sctp, OURS, 1);
    expect_request(1, OURS + 1, 299, rest, 2);
    assert(sctp->streams[5].flags & PEER_SCTP_STREAM_LOCAL_RESET);
    answer(sctp, OURS + 1, 1);
    at_ms(100000);
    assert(jetkvm_sctp_check_sack(sctp) == 7);
    assert(writes == 2 && sack_checks == 1);
    free(sctp);
}

/* A request goes out once the browser acknowledged every DATA chunk sent
 * before the close: it then performs it, and the release of a stream reset
 * both ways drops no DATA the browser still needs. The close is reported
 * at once. */
static void test_request_waits_for_acknowledged_data(int browser_first)
{
    peer_sctp_t *sctp = association();
    sctp->send_acked_tsn = 290;
    const uint16_t ids[] = {6};
    if (browser_first) {
        uint8_t chunk[64];
        size_t n = 4 + request(chunk + 4, 41, ids, 1);
        chunk_header(chunk, n);
        assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
        expect_response(0, 41);
    } else {
        close_stream(sctp, 6);
    }
    const unsigned before = writes;
    assert(closes == 1 && closed_ids[0] == 6);
    assert(sctp->streams[9].flags & PEER_SCTP_STREAM_OPEN);
    assert(!(sctp->streams[9].flags & PEER_SCTP_STREAM_LOCAL_RESET));
    sctp->send_tsn = 320;
    sctp->send_acked_tsn = 298;
    (void)jetkvm_sctp_check_sack(sctp);
    assert(writes == before);
    sctp->send_acked_tsn = 299;
    (void)jetkvm_sctp_check_sack(sctp);
    assert(writes == before + 1);
    expect_request(before, OURS, 299, ids, 1);
    assert(closes == 1);
    if (browser_first) {
        assert(sctp->streams[9].flags == 0);
    } else {
        assert(sctp->streams[9].flags & PEER_SCTP_STREAM_LOCAL_RESET);
    }
    free(sctp);
}

/* Our answer from the browser is lost, and the browser opens a new channel
 * on the stream once both directions are reset. Its close must not be taken
 * for the old one still in flight: it goes in the next request. */
static void test_close_of_reused_stream_while_request_in_flight(void)
{
    peer_sctp_t *sctp = association();
    const uint16_t ids[] = {6};
    close_stream(sctp, 6);
    expect_request(0, OURS, 299, ids, 1);
    uint8_t chunk[64];
    size_t n = 4 + request(chunk + 4, 41, ids, 1);
    chunk_header(chunk, n);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    expect_response(1, 41);
    assert(sctp->streams[9].flags == 0);
    static char reused[] = "guest-terminal";
    sctp->streams[9] = (peer_sctp_stream_t){.label = reused, .id = 6, .flags = 3};
    close_stream(sctp, 6);
    assert(closes == 2 && closed_ids[1] == 6 && writes == 2);
    answer(sctp, OURS, 1);
    expect_request(2, OURS + 1, 299, ids, 1);
    assert(sctp->streams[9].flags & PEER_SCTP_STREAM_LOCAL_RESET);
    free(sctp);
}

/* "In progress" restarts the timer of the request (RFC 6525 5.2.7). */
static void test_in_progress_request_is_repeated(void)
{
    peer_sctp_t *sctp = association();
    const uint16_t ids[] = {6};
    close_stream(sctp, 6);
    expect_request(0, OURS, 299, ids, 1);
    at_ms(1200);
    answer(sctp, OURS, 6);
    at_ms(1499);
    assert(jetkvm_sctp_check_sack(sctp) == 7 && writes == 1);
    at_ms(1500);
    assert(jetkvm_sctp_check_sack(sctp) == 7 && writes == 2);
    expect_request(1, OURS, 299, ids, 1);
    answer(sctp, OURS, 1);
    at_ms(100000);
    assert(jetkvm_sctp_check_sack(sctp) == 7);
    assert(writes == 2 && sack_checks == 3);
    free(sctp);
}

/* A request without an answer goes again, the timeout doubling up to a
 * minute; an answer to another request changes nothing, and any final
 * answer ends it. */
static void test_unanswered_request_is_repeated(void)
{
    peer_sctp_t *sctp = association();
    const uint16_t ids[] = {6};
    close_stream(sctp, 6);
    uint32_t due = 1300, timeout = 300;
    for (unsigned sent = 1; sent < 12; ++sent) {
        at_ms(due - 1);
        (void)jetkvm_sctp_check_sack(sctp);
        assert(writes == sent);
        answer(sctp, OURS - 1, 1);
        answer(sctp, OURS + 1, 1);
        at_ms(due);
        (void)jetkvm_sctp_check_sack(sctp);
        assert(writes == sent + 1);
        expect_request(sent, OURS, 299, ids, 1);
        timeout = timeout * 2 < 60000 ? timeout * 2 : 60000;
        due += timeout;
    }
    assert(timeout == 60000);
    answer(sctp, OURS, 2);
    at_ms(due);
    (void)jetkvm_sctp_check_sack(sctp);
    assert(writes == 12);
    free(sctp);
}

/* Both sides restart a reset stream at SSN 0 (RFC 6525 5.2.2). A channel
 * that reuses the slot must not continue the closed one's sequence numbers,
 * or the browser holds all its messages waiting for SSN 0. Closed from
 * either side. */
static void test_released_stream_restarts_sequences(int ours_first)
{
    peer_sctp_t *sctp = association();
    peer_sctp_stream_t *stream = &sctp->streams[9];
    stream->send_count = 5;
    stream->last_sent_sequence = 7;
    stream->flags |= PEER_SCTP_STREAM_FRAME_SENT;
    char label[PEER_SCTP_STREAM_LABEL_CAPACITY];
    peer_sctp_stream_label(label, 6);
    uint8_t chunk[64];
    const uint16_t ids[] = {6};
    size_t n = 4 + request(chunk + 4, 41, ids, 1);
    chunk_header(chunk, n);
    if (ours_first) assert(jetkvm_sctp_send_recfg(sctp, label) == 0);
    assert(jetkvm_sctp_handle_recfg(sctp, chunk) == 0);
    assert(closes == 1 && !(stream->flags & PEER_SCTP_STREAM_OPEN));
    assert(stream->send_count == 0 && stream->last_sent_sequence == 0);
    assert(!(stream->flags & PEER_SCTP_STREAM_FRAME_SENT));
    free(sctp);
}

/* A channel keeps its slot when a lower slot is freed: its next DATA chunk
 * must find it, not take the free slot. A new stream takes the first free
 * slot; none is left once all are taken. */
static void test_stream_lookup_after_lower_slot_freed(void)
{
    peer_sctp_t *sctp = association();
    sctp->stream_count = 4;
    assert(jetkvm_sctp_get_stream(sctp, 6) == &sctp->streams[9]);
    assert(sctp->streams[1].flags == 0 && sctp->stream_count == 4);
    peer_sctp_stream_t *fresh = jetkvm_sctp_get_stream(sctp, 8);
    assert(fresh == &sctp->streams[1] && fresh->id == 8 && fresh->index == 1);
    assert(fresh->flags == PEER_SCTP_STREAM_OPEN && sctp->stream_count == 5);
    assert(jetkvm_sctp_get_stream(sctp, 8) == fresh && sctp->stream_count == 5);
    for (uint16_t id = 10; id < 20; id += 2) {
        assert(jetkvm_sctp_get_stream(sctp, id) != NULL);
    }
    assert(sctp->stream_count == 10 && !jetkvm_sctp_get_stream(sctp, 20));
    assert(sctp->stream_count == 10);
    free(sctp);
}

/* A channel the browser opens while the send buffer is full: its ACK is
 * lost, so the Mini's first message on it must take SSN 0, which the
 * browser waits for. With room, the ACK takes SSN 0. */
static void test_refused_dcep_ack_takes_no_ssn(void)
{
    static char terminal[] = "guest-terminal";
    peer_sctp_t *sctp = association();
    sctp->streams[1] = (peer_sctp_stream_t){.label = terminal, .id = 8, .flags = 3};
    send_buffer_full = 1;
    jetkvm_sctp_send_dcep_ack(sctp, 8);
    assert(sctp->streams[1].send_count == 0 && sctp->send_tsn == 300);
    send_buffer_full = 0;
    sctp->streams[2] = (peer_sctp_stream_t){.label = terminal, .id = 10, .flags = 3};
    jetkvm_sctp_send_dcep_ack(sctp, 10);
    assert(sctp->streams[2].send_count == 1 && sctp->send_tsn == 301);
    assert(sctp->streams[1].send_count == 0);
    free(sctp);
}

int main(void)
{
    test_labels();
    test_browser_close();
    for (int order = 0; order < 3; ++order) test_local_close(order);
    test_many_streams();
    test_malformed();
    test_reset_before_last_data();
    test_repeated_request_after_reuse();
    test_one_request_in_flight();
    test_request_waits_for_acknowledged_data(0);
    test_request_waits_for_acknowledged_data(1);
    test_close_of_reused_stream_while_request_in_flight();
    test_in_progress_request_is_repeated();
    test_unanswered_request_is_repeated();
    test_released_stream_restarts_sequences(0);
    test_released_stream_restarts_sequences(1);
    test_stream_lookup_after_lower_slot_freed();
    test_refused_dcep_ack_takes_no_ssn();
    puts("SCTP stream reconfiguration: OK");
    return 0;
}
