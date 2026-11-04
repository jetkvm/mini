/* Shared layout for the SCTP core and channel adapters.
 * Channel slots and reset state are part of the source association layout. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct peer_sctp_frame {
    struct peer_sctp_frame *next;
    uint32_t sequence, min_tsn, max_tsn;
    uint16_t fill_count, ppid;
    uint8_t flags, stream_index;
} peer_sctp_frame_t;

typedef struct {
    char *label;
    uint16_t id;
    peer_sctp_frame_t *frames, *tail;
    uint32_t first_sequence, last_sequence, send_count, reconfiguration, index, type;
    uint16_t reliability_limit;
    uint8_t flags;
    uint32_t last_sent_sequence;
    void *direct_data;
    uint32_t direct_size, direct_capacity;
    uint16_t direct_ppid;
} peer_sctp_stream_t;

typedef struct peer_sctp_buffer {
    void *data;
    uint32_t size, allocated_size, add_time, sent_time, tsn, packet_id;
    uint16_t stream_id;
    uint8_t flags;
    uint16_t resend_count;
    struct peer_sctp_buffer *previous, *next;
} peer_sctp_buffer_t;

typedef struct {
    void *lock;
    uint8_t reserved[32];
    peer_sctp_buffer_t *free_list, *head, *tail;
    /* Bytes held (each buffer at its allocated size) and the cache size. */
    uint32_t fill_size, limit_size;
} peer_sctp_buffers_t;

typedef struct {
    uint8_t untouched[72];
    /* The sequence number of our next reset request. */
    uint32_t request_sequence;
    uint8_t untouched_76[16];
    /* The TSN of our next DATA chunk, and the browser's cumulative
     * acknowledgement of ours. */
    uint32_t send_tsn, send_acked_tsn;
    uint8_t untouched_100[1344];
    uint8_t stream_count;
    uint32_t acknowledged_tsn, latest_tsn;
    uint16_t received_packets;
    uint8_t sack_count;
    uint32_t sack_time;
    peer_sctp_buffers_t *receive_buffers;
    uint32_t forward_tsn;
    uint8_t forward_info[20];
    bool supports_forward, needs_forward, needs_abandon;
    uint32_t references;
    void *lock, *send_lock;
    peer_sctp_stream_t streams[10];
    /* The browser's last Outgoing SSN Reset Request and our answer (0 until
     * the first). */
    uint32_t reset_request, reset_result;
    /* Our Outgoing SSN Reset Request in flight (RFC 6525 5.1.1): its stream
     * IDs as bits (0 when none is in flight), sequence number and last
     * assigned TSN, its retransmission time and timeout; and the streams the
     * next request resets, with the last TSN assigned before they closed. */
    uint32_t sent_streams, sent_sequence, sent_last_tsn;
    uint32_t resend_at_ms, resend_timeout_ms;
    uint32_t queued_streams, queued_last_tsn;
} peer_sctp_t;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(peer_sctp_frame_t) == 24, "vendor frame layout");
_Static_assert(sizeof(peer_sctp_stream_t) == 64, "vendor stream layout");
_Static_assert(offsetof(peer_sctp_stream_t, flags) == 42, "vendor stream flags");
_Static_assert(offsetof(peer_sctp_buffer_t, flags) == 30, "vendor buffer flags");
_Static_assert(offsetof(peer_sctp_buffer_t, next) == 40, "vendor buffer links");
_Static_assert(offsetof(peer_sctp_buffers_t, head) == 40, "vendor buffer manager");
_Static_assert(offsetof(peer_sctp_buffers_t, limit_size) == 52, "vendor buffer limit");
_Static_assert(offsetof(peer_sctp_t, request_sequence) == 72, "vendor reset sequence");
_Static_assert(offsetof(peer_sctp_t, send_tsn) == 92, "vendor send TSN");
_Static_assert(offsetof(peer_sctp_t, send_acked_tsn) == 96, "vendor acknowledged send TSN");
_Static_assert(offsetof(peer_sctp_t, acknowledged_tsn) == 1448, "vendor receive TSN");
_Static_assert(offsetof(peer_sctp_t, sack_count) == 1458, "vendor SACK count");
_Static_assert(offsetof(peer_sctp_t, receive_buffers) == 1464, "vendor receive buffers");
_Static_assert(offsetof(peer_sctp_t, lock) == 1500, "vendor lock");
_Static_assert(offsetof(peer_sctp_t, streams) == 1508, "appended stream slots");
_Static_assert(sizeof(peer_sctp_t) == 2184, "adapted allocation size");
#endif

/* Stream flags (peer_sctp_stream_t.flags). */
#define PEER_SCTP_STREAM_OPEN 0x01U
/* The DCEP OPEN was reported to the application. */
#define PEER_SCTP_STREAM_ANNOUNCED 0x02U
/* Our outgoing stream was reset (sent, or the peer answered). */
#define PEER_SCTP_STREAM_LOCAL_RESET 0x04U
/* We answered the peer's reset of its outgoing stream. */
#define PEER_SCTP_STREAM_REMOTE_RESET 0x08U
/* The close callback ran. */
#define PEER_SCTP_STREAM_CLOSE_REPORTED 0x10U
/* A reliable frame was delivered; last_sent_sequence holds its SSN. */
#define PEER_SCTP_STREAM_FRAME_SENT 0x80U

/* The part of the receive cache that only the chunk after the cumulative
 * acknowledgement may use (peer_sctp_receive.c): the longest message the
 * browser may send, 160 KiB (the SDP's max-message-size, set in
 * the SCTP core), and 4 KiB for chunks whose buffer could
 * not be shrunk to their size. */
#define PEER_SCTP_RECEIVE_RESERVE (164U * 1024U)

/* Serial number order (RFC 1982) of TSNs and request sequence numbers. */
static inline bool peer_sctp_after(uint32_t a, uint32_t b)
{
    return a != b && (uint32_t)(a - b) < UINT32_C(0x80000000);
}

peer_sctp_frame_t *jetkvm_sctp_get_frame(peer_sctp_t *, peer_sctp_stream_t *, const void *);
void jetkvm_sctp_handle_forward_tsn(peer_sctp_t *, const void *);
int jetkvm_sctp_handle_recfg(peer_sctp_t *, const void *chunk);
int jetkvm_sctp_send_recfg(peer_sctp_t *, const char *label);
int jetkvm_sctp_check_sack(peer_sctp_t *);
peer_sctp_stream_t *jetkvm_sctp_get_stream(peer_sctp_t *, uint16_t id);
void jetkvm_sctp_send_dcep_ack(peer_sctp_t *, uint16_t id);
int jetkvm_sctp_receive_data(peer_sctp_t *, const void *chunk, int control);
uint32_t jetkvm_sctp_receive_window(peer_sctp_buffers_t *);
