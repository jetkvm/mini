/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * Bulk transfer policy for the OS Services interface, without TinyUSB, so the
 * host tests can run it against a model of USB packets.
 *
 * A bulk transfer completes when it is full or when a short packet (one
 * below wMaxPacketSize, including a zero-length packet) ends it. Neither
 * side sends a zero-length packet after a transfer of whole packets unless it
 * chooses to. Two rules follow:
 *
 * - OUT: a device read longer than what the sender has written waits for
 *   more data unless the sender's write ended with a short packet. The
 *   sender writes each message (in 64 KiB pieces) and may then wait for an
 *   answer, so a read must never span past the end of a message. The RX
 *   framer sizes each read from the message lengths in the stream.
 * - IN: the sender reads 64 KiB at a time. When the KVM's queue empties after
 *   a transfer of whole packets, the KVM sends a zero-length packet so the
 *   sender's read completes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sizes the OUT reads. The sender starts every message on a new transfer,
 * and every transfer that ends before the end of a message is a whole
 * number of packets. So a read that ends short ends a message, and the
 * first packet of a message holds its envelope.
 */
typedef struct {
    uint32_t packet_size;
    uint32_t read_capacity;
    /* The next byte starts a message. */
    bool at_boundary;
    /* Bytes left in the current message, when not at a boundary. */
    uint64_t remaining;
} osvc_rx_framer_t;

/* packet_size is wMaxPacketSize; read_capacity is a multiple of it. */
void osvc_rx_framer_init(osvc_rx_framer_t *framer, uint32_t packet_size,
                         uint32_t read_capacity);
/* The stream restarts at a message boundary: the endpoint was reopened. */
void osvc_rx_framer_restart(osvc_rx_framer_t *framer);
/* The length of the next read: one packet at a boundary, otherwise what is
 * left of the message in whole packets, at most read_capacity. */
uint32_t osvc_rx_framer_next_length(const osvc_rx_framer_t *framer);
/* A read of `requested` bytes completed with `received` bytes, data[0..]. */
void osvc_rx_framer_completed(osvc_rx_framer_t *framer, const uint8_t *data,
                              uint32_t requested, uint32_t received);

/*
 * The IN queue: a single-producer, single-consumer byte ring. The producer
 * (the session's task) appends whole messages and may flush the queue; the
 * consumer (the USB task) moves bytes into transfers. Indices are free
 * running; the capacity is a power of two.
 */
typedef struct {
    uint8_t *storage;
    uint32_t capacity;
    uint32_t packet_size;
    /* Written by the producer. */
    uint32_t head;
    uint32_t flush_head;
    uint32_t flush_sequence;
    /* Written by the consumer. */
    uint32_t tail;
    uint32_t seen_flush_sequence;
    uint32_t in_flight;
    uint32_t in_flight_flush_sequence;
    bool transfer_active;
    bool zero_length_due;
} osvc_tx_ring_t;

void osvc_tx_ring_init(osvc_tx_ring_t *ring, uint8_t *storage,
                       uint32_t capacity, uint32_t packet_size);

/* Producer. */
uint32_t osvc_tx_ring_free(const osvc_tx_ring_t *ring);
/* Appends the parts as one unit, or nothing when they do not fit. */
bool osvc_tx_ring_write(osvc_tx_ring_t *ring, const uint8_t *const parts[],
                        const size_t lengths[], size_t count);
/* Drops everything written so far. The consumer applies it at its next
 * call; a transfer already started still completes. */
void osvc_tx_ring_flush(osvc_tx_ring_t *ring);

/* Consumer. */
typedef enum {
    OSVC_TX_IDLE = 0,     /* nothing to send, or a transfer is active */
    OSVC_TX_DATA,         /* send *length bytes from the bounce buffer */
    OSVC_TX_ZERO_LENGTH,  /* send a zero-length packet */
} osvc_tx_action_t;

/* Starts the next transfer when none is active. */
osvc_tx_action_t osvc_tx_ring_start(osvc_tx_ring_t *ring, uint8_t *bounce,
                                    uint32_t bounce_capacity,
                                    uint32_t *length);
/* The active transfer completed. */
void osvc_tx_ring_completed(osvc_tx_ring_t *ring);
/* The endpoint closed with a transfer active: it will never complete. */
void osvc_tx_ring_abort(osvc_tx_ring_t *ring);
/* Bytes written and not yet completed, for the producer. */
uint32_t osvc_tx_ring_pending(const osvc_tx_ring_t *ring);

#ifdef __cplusplus
}
#endif
