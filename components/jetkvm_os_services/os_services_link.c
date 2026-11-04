/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_link.h"

#include <string.h>

#include "os_services_wire.h"

void osvc_rx_framer_init(osvc_rx_framer_t *framer, uint32_t packet_size,
                         uint32_t read_capacity)
{
    framer->packet_size = packet_size;
    framer->read_capacity = read_capacity;
    osvc_rx_framer_restart(framer);
}

void osvc_rx_framer_restart(osvc_rx_framer_t *framer)
{
    framer->at_boundary = true;
    framer->remaining = 0U;
}

uint32_t osvc_rx_framer_next_length(const osvc_rx_framer_t *framer)
{
    if (framer->at_boundary) {
        return framer->packet_size;
    }
    const uint64_t packets =
        (framer->remaining + framer->packet_size - 1U) / framer->packet_size;
    const uint64_t length = packets * framer->packet_size;
    return length < framer->read_capacity ? (uint32_t)length :
                                            framer->read_capacity;
}

void osvc_rx_framer_completed(osvc_rx_framer_t *framer, const uint8_t *data,
                              uint32_t requested, uint32_t received)
{
    if (framer->at_boundary) {
        /* The first packet of a message holds its envelope. Fewer bytes
         * than an envelope is a zero-length packet, or not a message. */
        if (received >= OSVC_ENVELOPE_LEN) {
            const uint64_t total =
                (uint64_t)OSVC_ENVELOPE_LEN + osvc_get_u32(data + 4);
            framer->remaining = total > received ? total - received : 0U;
        } else {
            framer->remaining = 0U;
        }
    } else {
        framer->remaining = received < framer->remaining ?
                                framer->remaining - received : 0U;
    }
    /* A short read ends the sender's write, and with it a message. Bytes
     * left over then were not a message, so the framer starts again. */
    framer->at_boundary = received < requested || framer->remaining == 0U;
    if (framer->at_boundary) {
        framer->remaining = 0U;
    }
}

static uint32_t load_u32(const uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void store_u32(uint32_t *value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

void osvc_tx_ring_init(osvc_tx_ring_t *ring, uint8_t *storage,
                       uint32_t capacity, uint32_t packet_size)
{
    memset(ring, 0, sizeof(*ring));
    ring->storage = storage;
    ring->capacity = capacity;
    ring->packet_size = packet_size;
}

uint32_t osvc_tx_ring_pending(const osvc_tx_ring_t *ring)
{
    return load_u32(&ring->head) - load_u32(&ring->tail);
}

uint32_t osvc_tx_ring_free(const osvc_tx_ring_t *ring)
{
    return ring->capacity - osvc_tx_ring_pending(ring);
}

static void copy_in(osvc_tx_ring_t *ring, uint32_t position,
                    const uint8_t *data, size_t length)
{
    const uint32_t mask = ring->capacity - 1U;
    while (length > 0U) {
        const uint32_t offset = position & mask;
        const size_t run = ring->capacity - offset < length ?
                               ring->capacity - offset : length;
        memcpy(ring->storage + offset, data, run);
        position += (uint32_t)run;
        data += run;
        length -= run;
    }
}

static void copy_out(const osvc_tx_ring_t *ring, uint32_t position,
                     uint8_t *out, uint32_t length)
{
    const uint32_t mask = ring->capacity - 1U;
    while (length > 0U) {
        const uint32_t offset = position & mask;
        const uint32_t run = ring->capacity - offset < length ?
                                 ring->capacity - offset : length;
        memcpy(out, ring->storage + offset, run);
        position += run;
        out += run;
        length -= run;
    }
}

bool osvc_tx_ring_write(osvc_tx_ring_t *ring, const uint8_t *const parts[],
                        const size_t lengths[], size_t count)
{
    uint64_t total = 0U;
    for (size_t i = 0U; i < count; ++i) {
        total += lengths[i];
    }
    if (total > osvc_tx_ring_free(ring)) {
        return false;
    }
    uint32_t head = ring->head;
    for (size_t i = 0U; i < count; ++i) {
        if (lengths[i] > 0U) {
            copy_in(ring, head, parts[i], lengths[i]);
            head += (uint32_t)lengths[i];
        }
    }
    store_u32(&ring->head, head);
    return true;
}

void osvc_tx_ring_flush(osvc_tx_ring_t *ring)
{
    store_u32(&ring->flush_head, ring->head);
    store_u32(&ring->flush_sequence, ring->flush_sequence + 1U);
}

static void apply_flush(osvc_tx_ring_t *ring)
{
    const uint32_t sequence = load_u32(&ring->flush_sequence);
    if (sequence == ring->seen_flush_sequence) {
        return;
    }
    ring->seen_flush_sequence = sequence;
    /* A later flush may have moved flush_head on already; it only drops
     * more of what was written before it. */
    store_u32(&ring->tail, load_u32(&ring->flush_head));
}

osvc_tx_action_t osvc_tx_ring_start(osvc_tx_ring_t *ring, uint8_t *bounce,
                                    uint32_t bounce_capacity,
                                    uint32_t *length)
{
    *length = 0U;
    apply_flush(ring);
    if (ring->transfer_active) {
        return OSVC_TX_IDLE;
    }
    const uint32_t pending = load_u32(&ring->head) - ring->tail;
    if (pending == 0U) {
        if (!ring->zero_length_due) {
            return OSVC_TX_IDLE;
        }
        ring->zero_length_due = false;
        ring->transfer_active = true;
        ring->in_flight = 0U;
        ring->in_flight_flush_sequence = ring->seen_flush_sequence;
        return OSVC_TX_ZERO_LENGTH;
    }
    const uint32_t take = pending < bounce_capacity ? pending : bounce_capacity;
    copy_out(ring, ring->tail, bounce, take);
    ring->transfer_active = true;
    ring->in_flight = take;
    ring->in_flight_flush_sequence = ring->seen_flush_sequence;
    ring->zero_length_due = false;
    *length = take;
    return OSVC_TX_DATA;
}

void osvc_tx_ring_completed(osvc_tx_ring_t *ring)
{
    if (!ring->transfer_active) {
        return;
    }
    ring->transfer_active = false;
    apply_flush(ring);
    /* A flush since the transfer started already moved the tail past it. */
    if (ring->in_flight_flush_sequence == ring->seen_flush_sequence) {
        store_u32(&ring->tail, ring->tail + ring->in_flight);
    }
    ring->zero_length_due = ring->in_flight > 0U &&
                            ring->in_flight % ring->packet_size == 0U;
    ring->in_flight = 0U;
}

void osvc_tx_ring_abort(osvc_tx_ring_t *ring)
{
    ring->transfer_active = false;
    ring->in_flight = 0U;
    ring->zero_length_due = false;
}
