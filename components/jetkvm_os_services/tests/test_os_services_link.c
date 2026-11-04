/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The bulk transfer policy against a model of USB bulk packets: a transfer
 * completes when it is full or a short packet ends it, and neither side adds
 * a zero-length packet unless the policy asks for one. The sender model
 * writes like the OS Services sender: one message per write, in 64 KiB
 * transfers, then possibly waits for an answer. The
 * property under test is that nothing written is left waiting in a
 * half-filled transfer whenever the writer pauses.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os_services_link.h"
#include "os_services_wire.h"

#define SENDER_TRANSFER (64U * 1024U)
#define SENDER_READ (64U * 1024U)
#define STREAM_CAPACITY (24U * 1024U * 1024U)

static uint64_t s_random = 0x2545f4914f6cdd1dULL;

static uint32_t next_random(void)
{
    s_random ^= s_random >> 12;
    s_random ^= s_random << 25;
    s_random ^= s_random >> 27;
    return (uint32_t)((s_random * 2685821657736338717ULL) >> 32);
}

/* Payload lengths that land the message on, next to, or away from packet
 * and transfer boundaries. */
static uint32_t pick_payload_length(uint32_t packet_size, uint32_t limit)
{
    const uint32_t envelope = OSVC_ENVELOPE_LEN;
    uint32_t length;
    switch (next_random() % 8U) {
    case 0:
        length = packet_size * (1U + next_random() % 8U) - envelope;
        break;
    case 1:
        length = SENDER_TRANSFER * (1U + next_random() % 4U) - envelope;
        break;
    case 2:
        length = packet_size * (1U + next_random() % 300U) - envelope +
                 (next_random() % 3U) - 1U;
        break;
    case 3:
        length = next_random() % 64U;
        break;
    case 4:
        length = SENDER_TRANSFER * (1U + next_random() % 3U) - envelope +
                 (next_random() % 3U) - 1U;
        break;
    default:
        length = next_random() % (3U * SENDER_TRANSFER);
        break;
    }
    return length > limit ? limit : length;
}

static size_t encode_message(uint8_t *out, uint16_t service, uint16_t kind,
                             uint32_t payload_length)
{
    osvc_put_envelope(out, service, kind, payload_length);
    for (uint32_t i = 0U; i < payload_length; ++i) {
        out[OSVC_ENVELOPE_LEN + i] = (uint8_t)next_random();
    }
    return OSVC_ENVELOPE_LEN + payload_length;
}

/* ---- OUT: sender writes, the device reads ---- */

typedef uint32_t (*read_policy_t)(void *ctx);
typedef void (*read_done_t)(void *ctx, const uint8_t *data, uint32_t requested,
                            uint32_t received);

typedef struct {
    uint32_t packet_size;
    read_policy_t policy;
    read_done_t done;
    void *policy_ctx;
    /* The device's open read. */
    uint8_t *read;
    uint32_t read_length;
    uint32_t read_fill;
    /* Everything the device has completed, in order. */
    uint8_t *delivered;
    size_t delivered_length;
    size_t reads_completed;
} out_link_t;

static void out_arm(out_link_t *link)
{
    link->read_length = link->policy(link->policy_ctx);
    assert(link->read_length > 0U);
    assert(link->read_length % link->packet_size == 0U);
    link->read_fill = 0U;
}

static void out_packet(out_link_t *link, const uint8_t *data, uint32_t length)
{
    assert(length <= link->packet_size);
    /* An open read always has room for a packet: it holds only whole
     * packets until it completes. */
    assert(link->read_fill + length <= link->read_length);
    memcpy(link->read + link->read_fill, data, length);
    link->read_fill += length;
    if (length < link->packet_size || link->read_fill == link->read_length) {
        memcpy(link->delivered + link->delivered_length, link->read,
               link->read_fill);
        link->delivered_length += link->read_fill;
        link->reads_completed++;
        link->done(link->policy_ctx, link->read, link->read_length,
                   link->read_fill);
        out_arm(link);
    }
}

/* One sender bulk write: whole packets, then a short one when the length is
 * not a multiple of the packet size. No zero-length packet. */
static void out_transfer(out_link_t *link, const uint8_t *data, size_t length)
{
    size_t offset = 0U;
    while (offset < length) {
        const size_t left = length - offset;
        const uint32_t packet =
            left < link->packet_size ? (uint32_t)left : link->packet_size;
        out_packet(link, data + offset, packet);
        offset += packet;
    }
}

/* The sender writes one message in 64 KiB transfers. */
static void out_message(out_link_t *link, const uint8_t *data, size_t length)
{
    for (size_t offset = 0U; offset < length; offset += SENDER_TRANSFER) {
        const size_t left = length - offset;
        out_transfer(link, data + offset,
                     left < SENDER_TRANSFER ? left : SENDER_TRANSFER);
    }
}

static uint32_t framer_policy(void *ctx)
{
    return osvc_rx_framer_next_length(ctx);
}

static void framer_done(void *ctx, const uint8_t *data, uint32_t requested,
                        uint32_t received)
{
    osvc_rx_framer_completed(ctx, data, requested, received);
}

typedef struct {
    uint32_t capacity;
} fixed_policy_t;

static uint32_t fixed_policy(void *ctx)
{
    return ((fixed_policy_t *)ctx)->capacity;
}

static void fixed_done(void *ctx, const uint8_t *data, uint32_t requested,
                       uint32_t received)
{
    (void)ctx;
    (void)data;
    (void)requested;
    (void)received;
}

static void out_link_init(out_link_t *link, uint32_t packet_size,
                          read_policy_t policy, read_done_t done, void *ctx,
                          uint32_t read_capacity)
{
    memset(link, 0, sizeof(*link));
    link->packet_size = packet_size;
    link->policy = policy;
    link->done = done;
    link->policy_ctx = ctx;
    link->read = malloc(read_capacity);
    link->delivered = malloc(STREAM_CAPACITY);
    assert(link->read && link->delivered);
    out_arm(link);
}

static void out_link_free(out_link_t *link)
{
    free(link->read);
    free(link->delivered);
}

static void test_framer_lengths(void)
{
    osvc_rx_framer_t framer;
    osvc_rx_framer_init(&framer, 512U, 16384U);
    assert(osvc_rx_framer_next_length(&framer) == 512U);

    /* A message of exactly two packets: the first read is one packet, the
     * second exactly the rest. */
    uint8_t packet[512] = {0};
    osvc_put_envelope(packet, 2U, 2U, 1024U - OSVC_ENVELOPE_LEN);
    osvc_rx_framer_completed(&framer, packet, 512U, 512U);
    assert(!framer.at_boundary);
    assert(framer.remaining == 512U);
    assert(osvc_rx_framer_next_length(&framer) == 512U);
    osvc_rx_framer_completed(&framer, packet, 512U, 512U);
    assert(framer.at_boundary);

    /* A large message reads in capacity-sized pieces, then the rest. */
    osvc_put_envelope(packet, 1U, 2U, 100000U);
    osvc_rx_framer_completed(&framer, packet, 512U, 512U);
    assert(osvc_rx_framer_next_length(&framer) == 16384U);
    for (int i = 0; i < 6; ++i) {
        osvc_rx_framer_completed(&framer, packet, 16384U, 16384U);
    }
    /* 100008 - 512 - 6 * 16384 = 1192 bytes: three packets. */
    assert(framer.remaining == 1192U);
    assert(osvc_rx_framer_next_length(&framer) == 1536U);
    osvc_rx_framer_completed(&framer, packet, 1536U, 1192U);
    assert(framer.at_boundary);

    /* A short read inside a message restarts at a boundary. */
    osvc_put_envelope(packet, 1U, 2U, 100000U);
    osvc_rx_framer_completed(&framer, packet, 512U, 512U);
    osvc_rx_framer_completed(&framer, packet, 16384U, 700U);
    assert(framer.at_boundary);
    assert(osvc_rx_framer_next_length(&framer) == 512U);

    /* A zero-length packet at a boundary is no message. */
    osvc_rx_framer_completed(&framer, packet, 512U, 0U);
    assert(framer.at_boundary);

    /* Full speed uses 64-byte packets. */
    osvc_rx_framer_init(&framer, 64U, 4096U);
    assert(osvc_rx_framer_next_length(&framer) == 64U);
    osvc_put_envelope(packet, 2U, 2U, 120U);
    osvc_rx_framer_completed(&framer, packet, 64U, 64U);
    assert(osvc_rx_framer_next_length(&framer) == 64U);
    assert(framer.remaining == 64U);
}

/* Runs a sender against a read policy. Returns the number of pauses at
 * which written bytes were still waiting in an open read. */
static size_t run_out(uint32_t packet_size, read_policy_t policy,
                      read_done_t done, void *ctx, uint32_t read_capacity,
                      bool stale_prefix, size_t message_count,
                      size_t *messages_checked)
{
    uint8_t *expected = malloc(STREAM_CAPACITY);
    uint8_t *message = malloc(OSVC_MAX_FRAME_PAYLOAD + OSVC_ENVELOPE_LEN);
    assert(expected && message);
    out_link_t link;
    out_link_init(&link, packet_size, policy, done, ctx, read_capacity);
    size_t expected_length = 0U;
    size_t stuck = 0U;

    if (stale_prefix) {
        /* Bytes of an earlier session: torn messages, garbage lengths,
         * writes of whole packets. */
        const size_t pieces = 1U + next_random() % 6U;
        for (size_t i = 0U; i < pieces; ++i) {
            size_t length = (next_random() % 4U == 0U) ?
                                packet_size * (1U + next_random() % 40U) :
                                1U + next_random() % 9000U;
            for (size_t j = 0U; j < length; ++j) {
                message[j] = (uint8_t)next_random();
            }
            out_transfer(&link, message, length);
            memcpy(expected + expected_length, message, length);
            expected_length += length;
        }
        /* The hello always ends with a short packet, so the stale bytes
         * and the hello are both delivered now. */
        const size_t length = encode_message(message, OSVC_SERVICE_SESSION,
                                             OSVC_SESSION_HELLO,
                                             OSVC_HELLO_LEN);
        out_message(&link, message, length);
        memcpy(expected + expected_length, message, length);
        expected_length += length;
        if (link.read_fill != 0U) {
            stuck++;
        }
    }

    for (size_t i = 0U; i < message_count; ++i) {
        const bool frame = next_random() % 5U == 0U;
        const uint32_t limit = frame ? 600U * 1024U : OSVC_MAX_CONTROL_PAYLOAD;
        const uint32_t payload = pick_payload_length(packet_size, limit);
        const size_t length = encode_message(
            message, frame ? OSVC_SERVICE_VIDEO : OSVC_SERVICE_CLIPBOARD,
            frame ? OSVC_VIDEO_FRAME : OSVC_CLIPBOARD_CONTENT, payload);
        if (expected_length + length > STREAM_CAPACITY) {
            break;
        }
        out_message(&link, message, length);
        memcpy(expected + expected_length, message, length);
        expected_length += length;
        /* The sender may now wait for an answer: the whole message must
         * have reached the device. */
        if (link.read_fill != 0U ||
            link.delivered_length != expected_length) {
            stuck++;
        }
        (*messages_checked)++;
    }

    /* Whatever the policy, every byte arrives in order eventually. */
    assert(link.delivered_length + link.read_fill == expected_length);
    assert(memcmp(link.delivered, expected, link.delivered_length) == 0);
    out_link_free(&link);
    free(expected);
    free(message);
    return stuck;
}

static void test_out_framer_never_strands_a_message(void)
{
    static const uint32_t packet_sizes[] = {512U, 64U};
    size_t checked = 0U;
    for (size_t p = 0U; p < 2U; ++p) {
        for (int round = 0; round < 40; ++round) {
            const uint32_t capacity = packet_sizes[p] == 512U ? 16384U : 4096U;
            osvc_rx_framer_t framer;
            osvc_rx_framer_init(&framer, packet_sizes[p], capacity);
            const size_t stuck = run_out(
                packet_sizes[p], framer_policy, framer_done, &framer,
                capacity, round % 2 == 1, 60U, &checked);
            assert(stuck == 0U);
        }
    }
    assert(checked > 4000U);
}

/* The same model catches the failure the framer exists to avoid: a reader
 * that always asks for 16 KiB strands messages that end on a packet
 * boundary. */
static void test_out_model_detects_stranding(void)
{
    fixed_policy_t fixed = {.capacity = 16384U};
    size_t checked = 0U;
    const size_t stuck = run_out(512U, fixed_policy, fixed_done, &fixed,
                                 16384U, false, 400U, &checked);
    assert(stuck > 0U);

    /* The smallest case: a 1024-byte clipboard message. */
    out_link_t link;
    out_link_init(&link, 512U, fixed_policy, fixed_done, &fixed, 16384U);
    uint8_t message[1024];
    const size_t length = encode_message(message, OSVC_SERVICE_CLIPBOARD,
                                         OSVC_CLIPBOARD_CONTENT,
                                         1024U - OSVC_ENVELOPE_LEN);
    out_message(&link, message, length);
    assert(link.delivered_length == 0U && link.read_fill == 1024U);
    out_link_free(&link);

    osvc_rx_framer_t framer;
    osvc_rx_framer_init(&framer, 512U, 16384U);
    out_link_init(&link, 512U, framer_policy, framer_done, &framer, 16384U);
    out_message(&link, message, length);
    assert(link.delivered_length == 1024U && link.read_fill == 0U);
    out_link_free(&link);
}

/* ---- IN: the device writes, the sender reads 64 KiB at a time ---- */

typedef struct {
    uint32_t packet_size;
    uint8_t *read;
    uint32_t read_fill;
    uint8_t *delivered;
    size_t delivered_length;
} in_host_t;

static void in_host_packet(in_host_t *host, const uint8_t *data,
                           uint32_t length)
{
    memcpy(host->read + host->read_fill, data, length);
    host->read_fill += length;
    if (length < host->packet_size || host->read_fill == SENDER_READ) {
        memcpy(host->delivered + host->delivered_length, host->read,
               host->read_fill);
        host->delivered_length += host->read_fill;
        host->read_fill = 0U;
    }
}

static void in_host_transfer(in_host_t *host, const uint8_t *data,
                             uint32_t length)
{
    if (length == 0U) {
        in_host_packet(host, data, 0U);
        return;
    }
    for (uint32_t offset = 0U; offset < length; offset += host->packet_size) {
        const uint32_t left = length - offset;
        in_host_packet(host, data + offset,
                       left < host->packet_size ? left : host->packet_size);
    }
}

/* Runs the consumer until the queue is idle. */
static void in_drain(osvc_tx_ring_t *ring, in_host_t *host, uint8_t *bounce,
                     uint32_t bounce_capacity, bool honour_zero_length)
{
    for (;;) {
        uint32_t length;
        const osvc_tx_action_t action =
            osvc_tx_ring_start(ring, bounce, bounce_capacity, &length);
        if (action == OSVC_TX_IDLE) {
            return;
        }
        if (action == OSVC_TX_DATA) {
            in_host_transfer(host, bounce, length);
        } else if (honour_zero_length) {
            in_host_transfer(host, bounce, 0U);
        }
        osvc_tx_ring_completed(ring);
    }
}

static size_t write_message(osvc_tx_ring_t *ring, uint8_t *scratch,
                            uint32_t payload)
{
    const size_t length = encode_message(scratch, OSVC_SERVICE_FILE,
                                         OSVC_FILE_CHUNK, payload);
    const uint8_t *parts[] = {scratch, scratch + OSVC_ENVELOPE_LEN};
    const size_t lengths[] = {OSVC_ENVELOPE_LEN, payload};
    assert(osvc_tx_ring_write(ring, parts, lengths, 2U));
    return length;
}

static size_t run_in(uint32_t packet_size, bool honour_zero_length,
                     size_t messages)
{
    const uint32_t capacity = 256U * 1024U;
    uint8_t *storage = malloc(capacity);
    uint8_t *scratch = malloc(OSVC_MAX_CONTROL_PAYLOAD + OSVC_ENVELOPE_LEN);
    uint8_t *expected = malloc(STREAM_CAPACITY);
    uint8_t bounce[16384];
    in_host_t host = {
        .packet_size = packet_size,
        .read = malloc(SENDER_READ),
        .delivered = malloc(STREAM_CAPACITY),
    };
    assert(storage && scratch && expected && host.read && host.delivered);
    osvc_tx_ring_t ring;
    osvc_tx_ring_init(&ring, storage, capacity, packet_size);
    size_t expected_length = 0U;
    size_t stuck = 0U;
    for (size_t i = 0U; i < messages; ++i) {
        const uint32_t payload =
            pick_payload_length(packet_size, OSVC_MAX_CONTROL_PAYLOAD);
        /* A full queue makes the writer wait for the USB task. */
        if (osvc_tx_ring_free(&ring) < OSVC_ENVELOPE_LEN + payload) {
            in_drain(&ring, &host, bounce, sizeof(bounce),
                     honour_zero_length);
        }
        const size_t length = write_message(&ring, scratch, payload);
        memcpy(expected + expected_length, scratch, length);
        expected_length += length;
        /* Sometimes several messages queue before the USB task runs. */
        if (next_random() % 3U != 0U) {
            continue;
        }
        in_drain(&ring, &host, bounce, sizeof(bounce), honour_zero_length);
        assert(osvc_tx_ring_pending(&ring) == 0U);
        if (host.read_fill != 0U || host.delivered_length != expected_length) {
            stuck++;
        }
    }
    in_drain(&ring, &host, bounce, sizeof(bounce), honour_zero_length);
    assert(host.delivered_length + host.read_fill == expected_length);
    assert(memcmp(host.delivered, expected, host.delivered_length) == 0);
    free(storage);
    free(scratch);
    free(expected);
    free(host.read);
    free(host.delivered);
    return stuck;
}

static void test_in_queue_never_strands_a_message(void)
{
    for (int round = 0; round < 20; ++round) {
        assert(run_in(512U, true, 200U) == 0U);
        assert(run_in(64U, true, 100U) == 0U);
    }
    /* Without the zero-length packet the sender's 64 KiB read waits. */
    size_t stuck = 0U;
    for (int round = 0; round < 5; ++round) {
        stuck += run_in(512U, false, 200U);
    }
    assert(stuck > 0U);
}

static void test_in_queue_write_is_atomic_and_bounded(void)
{
    uint8_t storage[1024];
    osvc_tx_ring_t ring;
    osvc_tx_ring_init(&ring, storage, sizeof(storage), 64U);
    uint8_t data[1024];
    for (size_t i = 0U; i < sizeof(data); ++i) {
        data[i] = (uint8_t)i;
    }
    const uint8_t *parts[] = {data, data + 500};
    size_t lengths[] = {500U, 500U};
    assert(osvc_tx_ring_write(&ring, parts, lengths, 2U));
    assert(osvc_tx_ring_free(&ring) == 24U);
    /* A message that does not fit leaves the queue as it was. */
    lengths[0] = 20U;
    lengths[1] = 5U;
    assert(!osvc_tx_ring_write(&ring, parts, lengths, 2U));
    assert(osvc_tx_ring_pending(&ring) == 1000U);

    /* Bytes wrap around the end of the storage. */
    uint8_t bounce[768];
    uint32_t length;
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_DATA);
    assert(length == 768U && memcmp(bounce, data, 768U) == 0);
    /* One transfer at a time. */
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_IDLE);
    osvc_tx_ring_completed(&ring);
    lengths[0] = 300U;
    lengths[1] = 400U;
    assert(osvc_tx_ring_write(&ring, parts, lengths, 2U));
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_DATA);
    assert(length == 768U);
    assert(memcmp(bounce, data + 768, 232U) == 0);
    assert(memcmp(bounce + 232, data, 300U) == 0);
    assert(memcmp(bounce + 532, data + 500, 236U) == 0);
    osvc_tx_ring_completed(&ring);
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_DATA);
    assert(length == 164U && memcmp(bounce, data + 736, 164U) == 0);
    osvc_tx_ring_completed(&ring);
    /* 164 is not a multiple of 64: no zero-length packet. */
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_IDLE);
}

static void test_in_queue_flush(void)
{
    uint8_t storage[4096];
    osvc_tx_ring_t ring;
    osvc_tx_ring_init(&ring, storage, sizeof(storage), 64U);
    uint8_t old_session[1000];
    uint8_t new_session[100];
    memset(old_session, 0xaa, sizeof(old_session));
    memset(new_session, 0x55, sizeof(new_session));
    const uint8_t *parts[] = {old_session};
    size_t lengths[] = {sizeof(old_session)};
    assert(osvc_tx_ring_write(&ring, parts, lengths, 1U));

    /* A transfer of the old session is on the bus when the reset comes. */
    uint8_t bounce[256];
    uint32_t length;
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_DATA);
    assert(length == 256U);
    osvc_tx_ring_flush(&ring);
    parts[0] = new_session;
    lengths[0] = sizeof(new_session);
    assert(osvc_tx_ring_write(&ring, parts, lengths, 1U));
    /* The rest of the old session is gone; only the new message is left
     * once the old transfer completes. */
    osvc_tx_ring_completed(&ring);
    assert(osvc_tx_ring_pending(&ring) == sizeof(new_session));
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_DATA);
    assert(length == sizeof(new_session));
    assert(memcmp(bounce, new_session, sizeof(new_session)) == 0);
    osvc_tx_ring_completed(&ring);
    assert(osvc_tx_ring_pending(&ring) == 0U);

    /* A flush with no transfer on the bus applies at the next start. */
    parts[0] = old_session;
    lengths[0] = sizeof(old_session);
    assert(osvc_tx_ring_write(&ring, parts, lengths, 1U));
    osvc_tx_ring_flush(&ring);
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_IDLE);
    assert(osvc_tx_ring_pending(&ring) == 0U);

    /* The endpoint closing drops the transfer on the bus. */
    assert(osvc_tx_ring_write(&ring, parts, lengths, 1U));
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_DATA);
    osvc_tx_ring_abort(&ring);
    osvc_tx_ring_flush(&ring);
    assert(osvc_tx_ring_start(&ring, bounce, sizeof(bounce), &length) ==
           OSVC_TX_IDLE);
    assert(osvc_tx_ring_free(&ring) == sizeof(storage));
}

int main(void)
{
    test_framer_lengths();
    test_out_framer_never_strands_a_message();
    test_out_model_detects_stranding();
    test_in_queue_write_is_atomic_and_bounded();
    test_in_queue_flush();
    test_in_queue_never_strands_a_message();
    puts("os_services link tests passed");
    return 0;
}
