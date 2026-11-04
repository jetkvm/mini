/* SCTP stream reconfiguration (RFC 6525).
 *
 * A DataChannel closes when both of its SCTP streams are reset (RFC 8831):
 * the side that closes resets its outgoing stream, and the other side
 * answers and resets its own. Espressif's esp_peer 1.5.5 RE-CONFIG handling,
 * which this file replaces, does not match what the browser expects:
 *
 * - It reads only the first parameter of a RE-CONFIG chunk, and only the
 *   first stream of an Outgoing SSN Reset Request.
 * - Its response carries 16 bits of the request's sequence number. The
 *   browser ignores a response it cannot match and keeps the request
 *   pending, which holds every later reset of the association behind it.
 * - It answers "nothing to do", which Firefox reports as a failed reset.
 * - It neither resets its own outgoing stream nor reports the close, so the
 *   channel never finishes closing on either side and its slot is never
 *   freed.
 * - It matches a response to our request against every open stream, so a
 *   late one can close a channel that reused the slot.
 * - It sends one request per closed stream at once and never repeats one.
 *   A peer that answers "in progress" (the data sent before the request had
 *   not all arrived) waits for the request again, and a lost one leaves
 *   every later request out of sequence; either way the channel never
 *   finishes closing in the browser.
 * - Its release of a stream reset both ways drops the stream's DATA that
 *   the browser has not acknowledged. When the browser closes first, a lost
 *   chunk is never sent again: the browser's cumulative acknowledgement
 *   stops there for the rest of the session, and it answers every reset of
 *   the stream "in progress".
 * - It resets a stream before the data sent ahead of the request arrived,
 *   and again when a late copy of the request arrives.
 *
 * third_party/esp_peer/src/core/sctp.c sends sctp_incoming_data's RE-CONFIG
 * chunk here (jetkvm_sctp_handle_recfg), sctp_close_channel's reset
 * (jetkvm_sctp_send_recfg), which also resolves the stream labels of
 * peer_sctp_streams.h, every stream lookup that can take a free slot
 * (jetkvm_sctp_get_stream), and the SACK check of every main loop pass
 * (jetkvm_sctp_check_sack), which sends and times our reset request. Our
 * close reports the close at once, through jetkvm_vendor_stream_closed().
 * The request goes out when none is in flight and the browser acknowledged
 * every DATA chunk sent before the close, with every stream closed since
 * the last one; only then is a stream marked reset, so its release drops
 * nothing. Everything runs on the task that runs esp_peer_main_loop(), as
 * the rest of esp_peer does; the close callback runs inside these calls and
 * must not close another channel.
 */
#include "peer_sctp_hid_internal.h"
#include "peer_sctp_streams.h"

#include <string.h>

/* Defined in third_party/esp_peer/src; not in its public headers. */
extern int media_lib_mutex_lock(void *, uint32_t);
extern int media_lib_mutex_unlock(void *);
extern int jetkvm_vendor_stream_closed(peer_sctp_t *, peer_sctp_stream_t *);
extern int jetkvm_vendor_fill_header_and_write(peer_sctp_t *, void *packet,
                                               int length);
extern int jetkvm_vendor_check_sack(peer_sctp_t *);
extern void jetkvm_vendor_send_dcep_ack(peer_sctp_t *, uint16_t id);
extern int64_t esp_timer_get_time(void);

#define STREAM_SLOTS \
    (sizeof(((peer_sctp_t *)0)->streams) / sizeof(((peer_sctp_t *)0)->streams[0]))
/* The vendor's outgoing packet buffer (common header first), which its send
 * lock guards and its writer always sends from. */
#define PACKET_OFFSET 100U
#define COMMON_HEADER 12U
#define CHUNK_RECONFIG 130U
#define PARAM_OUTGOING_RESET 13U
#define PARAM_RESPONSE 16U
#define RESULT_PERFORMED 1U
#define RESULT_BAD_SEQUENCE 5U
#define RESULT_IN_PROGRESS 6U
/* Type, length and the three sequence numbers; stream numbers follow. */
#define OUTGOING_RESET_FIXED 16U
/* Type, length, sequence number and result. */
#define RESPONSE_LENGTH 12U
/* The vendor's DATA retransmission interval; doubled on every expiry
 * (RFC 6525 5.1.1) up to RTO.Max (RFC 9260). */
#define RESET_TIMEOUT_MS 300U
#define RESET_TIMEOUT_MAX_MS 60000U

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static peer_sctp_stream_t *open_stream(peer_sctp_t *sctp, uint16_t id)
{
    for (size_t i = 0; i < STREAM_SLOTS; ++i) {
        peer_sctp_stream_t *stream = &sctp->streams[i];
        if ((stream->flags & PEER_SCTP_STREAM_OPEN) && stream->id == id) {
            return stream;
        }
    }
    return NULL;
}

/* Answers a reset request, with its whole sequence number. */
static void send_response(peer_sctp_t *sctp, uint32_t sequence, uint32_t result)
{
    static const uint8_t head[8] = {
        CHUNK_RECONFIG, 0, 0, 4U + RESPONSE_LENGTH, 0, PARAM_RESPONSE, 0,
        RESPONSE_LENGTH,
    };
    uint8_t *packet = (uint8_t *)sctp + PACKET_OFFSET;
    media_lib_mutex_lock(sctp->send_lock, UINT32_MAX);
    uint8_t *chunk = packet + COMMON_HEADER;
    memcpy(chunk, head, sizeof(head));
    put_be32(chunk + 8, sequence);
    put_be32(chunk + 12, result);
    (void)jetkvm_vendor_fill_header_and_write(
        sctp, packet, (int)(COMMON_HEADER + 4U + RESPONSE_LENGTH));
    media_lib_mutex_unlock(sctp->send_lock);
}

/* Every call to the vendor's get_stream comes here: the slot of a DATA
 * chunk's or DCEP message's stream, or a new one. The vendor takes the first
 * free slot it meets before it looks at the slots after it. Once a channel
 * in a lower slot closed, the next DATA chunk of a channel in a higher slot
 * took a second slot for the same stream, without a label and at SSN 0: the
 * Mini sent that channel's next messages from it, which the browser held
 * waiting for the SSN it expected, and could not close the channel. */
peer_sctp_stream_t *jetkvm_sctp_get_stream(peer_sctp_t *sctp, uint16_t id)
{
    peer_sctp_stream_t *free_slot = NULL;
    for (size_t i = 0; i < STREAM_SLOTS; ++i) {
        peer_sctp_stream_t *stream = &sctp->streams[i];
        if (!(stream->flags & PEER_SCTP_STREAM_OPEN)) {
            if (!free_slot) free_slot = stream;
        } else if (stream->id == id) {
            return stream;
        }
    }
    if (free_slot) {
        free_slot->index = (uint32_t)(free_slot - sctp->streams);
        free_slot->id = id;
        free_slot->flags |= PEER_SCTP_STREAM_OPEN;
        sctp->stream_count++;
    }
    return free_slot;
}

/* Both sides restart a reset stream at SSN 0 (RFC 6525 5.2.2). The vendor's
 * release keeps the stream's outgoing SSN, the last SSN it delivered and
 * the flag that turns that one on, so a channel that reused the slot went
 * on from the closed one's numbers and the browser held all its messages
 * waiting for SSN 0. Called after every vendor call that can release it. */
static void restart_if_released(peer_sctp_stream_t *stream)
{
    if (stream->flags & PEER_SCTP_STREAM_OPEN) return;
    stream->send_count = 0;
    stream->last_sent_sequence = 0;
    stream->flags &= (uint8_t)~PEER_SCTP_STREAM_FRAME_SENT;
}

/* Sends the request in flight, again when it is repeated: an Outgoing SSN
 * Reset Request of every stream in sent_streams. */
static void send_request(peer_sctp_t *sctp)
{
    uint8_t *packet = (uint8_t *)sctp + PACKET_OFFSET;
    media_lib_mutex_lock(sctp->send_lock, UINT32_MAX);
    uint8_t *param = packet + COMMON_HEADER + 4U;
    size_t length = OUTGOING_RESET_FIXED;
    for (uint16_t id = 0; id < 32U; ++id) {
        if (sctp->sent_streams & (UINT32_C(1) << id)) {
            put_be16(param + length, id);
            length += 2U;
        }
    }
    put_be16(param, PARAM_OUTGOING_RESET);
    put_be16(param + 2, (uint16_t)length);
    put_be32(param + 4, sctp->sent_sequence);
    /* The response sequence number only answers an Incoming SSN Reset
     * Request, which we never get. */
    put_be32(param + 8, 0);
    put_be32(param + 12, sctp->sent_last_tsn);
    uint8_t *chunk = packet + COMMON_HEADER;
    chunk[0] = CHUNK_RECONFIG;
    chunk[1] = 0;
    put_be16(chunk + 2, (uint16_t)(4U + length));
    const size_t padded = (4U + length + 3U) & ~(size_t)3U;
    memset(chunk + 4U + length, 0, padded - 4U - length);
    (void)jetkvm_vendor_fill_header_and_write(sctp, packet,
                                              (int)(COMMON_HEADER + padded));
    media_lib_mutex_unlock(sctp->send_lock);
}

/* Marks each stream of the request reset, which frees its slot when the
 * browser's stream is reset too. No slot reused a stream ID in it: the
 * browser reuses one only once this request is performed. */
static void mark_sent_streams_reset(peer_sctp_t *sctp)
{
    for (uint16_t id = 0; id < 32U; ++id) {
        if (!(sctp->sent_streams & (UINT32_C(1) << id))) continue;
        peer_sctp_stream_t *stream = open_stream(sctp, id);
        if (!stream) continue;
        stream->flags |= PEER_SCTP_STREAM_LOCAL_RESET;
        (void)jetkvm_vendor_stream_closed(sctp, stream);
        restart_if_released(stream);
    }
}

/* One request at a time (RFC 6525 5.1.1), with every stream queued while
 * the last was in flight, once the browser acknowledged every DATA chunk
 * sent before they closed: then it performs the request, and the release
 * of a stream drops none of its DATA. */
static void send_queued(peer_sctp_t *sctp)
{
    if (sctp->sent_streams || !sctp->queued_streams ||
        peer_sctp_after(sctp->queued_last_tsn, sctp->send_acked_tsn)) {
        return;
    }
    sctp->sent_streams = sctp->queued_streams;
    sctp->queued_streams = 0;
    sctp->sent_sequence = sctp->request_sequence++;
    sctp->sent_last_tsn = sctp->queued_last_tsn;
    sctp->resend_timeout_ms = RESET_TIMEOUT_MS;
    sctp->resend_at_ms = now_ms() + RESET_TIMEOUT_MS;
    send_request(sctp);
    mark_sent_streams_reset(sctp);
}

/* Resets our outgoing stream once and reports the close. The stream may be
 * gone when it returns. A stream in the request in flight is marked reset;
 * the ID there without the mark is a new channel in a reused slot, whose
 * close waits for the next request. */
static int reset_outgoing(peer_sctp_t *sctp, peer_sctp_stream_t *stream)
{
    /* INIT and INIT ACK announce 20 streams, so no ID is 32 or more. */
    if (stream->id >= 32U) return -1;
    const uint32_t bit = UINT32_C(1) << stream->id;
    if ((stream->flags & PEER_SCTP_STREAM_LOCAL_RESET) ||
        (sctp->queued_streams & bit)) {
        return 0;
    }
    sctp->queued_streams |= bit;
    sctp->queued_last_tsn = sctp->send_tsn - 1U;
    (void)jetkvm_vendor_stream_closed(sctp, stream);
    send_queued(sctp);
    return 0;
}

/* The browser reset its outgoing stream: reset ours too, which reports the
 * close, or free the slot when ours was reset first. */
static void close_remote(peer_sctp_t *sctp, peer_sctp_stream_t *stream)
{
    if (stream->flags & PEER_SCTP_STREAM_REMOTE_RESET) return;
    stream->flags |= PEER_SCTP_STREAM_REMOTE_RESET;
    if (stream->flags & PEER_SCTP_STREAM_LOCAL_RESET) {
        (void)jetkvm_vendor_stream_closed(sctp, stream);
        restart_if_released(stream);
    } else {
        /* Ours is reset, or will be once its request goes out. */
        (void)reset_outgoing(sctp, stream);
    }
}

static void close_listed(peer_sctp_t *sctp, const uint8_t *param,
                         size_t length)
{
    const size_t count = (length - OUTGOING_RESET_FIXED) / 2U;
    const uint8_t *ids = param + OUTGOING_RESET_FIXED;
    if (count == 0U) {
        /* No stream listed resets every stream (RFC 6525 4.1). */
        for (size_t i = 0; i < STREAM_SLOTS; ++i) {
            if (sctp->streams[i].flags & PEER_SCTP_STREAM_OPEN) {
                close_remote(sctp, &sctp->streams[i]);
            }
        }
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        peer_sctp_stream_t *stream = open_stream(sctp, be16(ids + 2U * i));
        if (stream) close_remote(sctp, stream);
    }
}

/* RFC 6525 5.2: the last request again is answered as before, unless it
 * waited for data; an older one is stale. A new request waits, answered
 * "in progress" until the browser repeats it, while DATA sent before it
 * is missing. */
static void answer_outgoing_reset(peer_sctp_t *sctp, const uint8_t *param,
                                  size_t length)
{
    const uint32_t sequence = be32(param + 4);
    const bool repeat = sctp->reset_result && sequence == sctp->reset_request;
    if (sctp->reset_result && !peer_sctp_after(sequence, sctp->reset_request) &&
        !(repeat && sctp->reset_result == RESULT_IN_PROGRESS)) {
        send_response(sctp, sequence,
                      repeat ? sctp->reset_result : RESULT_BAD_SEQUENCE);
        return;
    }
    sctp->reset_request = sequence;
    sctp->reset_result = peer_sctp_after(be32(param + 12), sctp->acknowledged_tsn)
                             ? RESULT_IN_PROGRESS : RESULT_PERFORMED;
    send_response(sctp, sequence, sctp->reset_result);
    if (sctp->reset_result == RESULT_PERFORMED) close_listed(sctp, param, length);
}

/* The browser's answer to our request in flight. "In progress" restarts
 * its timer (RFC 6525 5.2.7); any other answer ends it, as nothing can make
 * a denied or out-of-sequence request succeed. */
static void answer_response(peer_sctp_t *sctp, const uint8_t *param)
{
    if (!sctp->sent_streams || be32(param + 4) != sctp->sent_sequence) return;
    if (be32(param + 8) == RESULT_IN_PROGRESS) {
        sctp->resend_at_ms = now_ms() + sctp->resend_timeout_ms;
        return;
    }
    sctp->sent_streams = 0;
    send_queued(sctp);
}

int jetkvm_sctp_handle_recfg(peer_sctp_t *sctp, const void *chunk)
{
    const uint8_t *wire = chunk;
    /* The caller checked the chunk's length against the packet. */
    const size_t length = be16(wire + 2);
    size_t offset = 4U;
    while (length >= offset + 4U) {
        const uint8_t *param = wire + offset;
        const size_t param_length = be16(param + 2);
        if (param_length < 4U || param_length > length - offset) break;
        const uint16_t type = be16(param);
        if (type == PARAM_OUTGOING_RESET &&
            param_length >= OUTGOING_RESET_FIXED) {
            answer_outgoing_reset(sctp, param, param_length);
        } else if (type == PARAM_RESPONSE && param_length >= RESPONSE_LENGTH) {
            answer_response(sctp, param);
        }
        /* The requests the vendor never handled (incoming reset, TSN reset,
         * added streams) are ignored. */
        offset += (param_length + 3U) & ~(size_t)3U;
    }
    return 0;
}

int jetkvm_sctp_send_recfg(peer_sctp_t *sctp, const char *label)
{
    uint16_t id;
    peer_sctp_stream_t *stream = NULL;
    if (peer_sctp_parse_stream_label(label, &id)) {
        stream = open_stream(sctp, id);
    } else {
        /* The vendor's rule for any other label: the first open stream. */
        for (size_t i = 0; i < STREAM_SLOTS && !stream && label; ++i) {
            peer_sctp_stream_t *candidate = &sctp->streams[i];
            if ((candidate->flags & PEER_SCTP_STREAM_OPEN) &&
                candidate->label && strcmp(candidate->label, label) == 0) {
                stream = candidate;
            }
        }
    }
    return stream ? reset_outgoing(sctp, stream) : -1;
}

/* Every main loop pass: the vendor's SACK check, then the queued request
 * once its DATA is acknowledged, or the one in flight again when its timer
 * ran out. */
int jetkvm_sctp_check_sack(peer_sctp_t *sctp)
{
    const int result = jetkvm_vendor_check_sack(sctp);
    send_queued(sctp);
    const uint32_t now = now_ms();
    if (sctp->sent_streams && (int32_t)(now - sctp->resend_at_ms) >= 0) {
        sctp->resend_timeout_ms =
            sctp->resend_timeout_ms < RESET_TIMEOUT_MAX_MS / 2U
                ? sctp->resend_timeout_ms * 2U : RESET_TIMEOUT_MAX_MS;
        sctp->resend_at_ms = now + sctp->resend_timeout_ms;
        send_request(sctp);
    }
    return result;
}

/* sctp_handle_dcep's DATA_CHANNEL_ACK for a channel the browser opened. The
 * vendor counts the ACK in the stream's SSNs also when its full send buffer
 * refused it. The browser then waited for that SSN and delivered nothing the
 * Mini sent on the channel. A refused ACK now takes no SSN: the browser takes
 * the channel's first message as its ACK (RFC 8832, section 6). */
void jetkvm_sctp_send_dcep_ack(peer_sctp_t *sctp, uint16_t id)
{
    const uint32_t tsn = sctp->send_tsn;
    jetkvm_vendor_send_dcep_ack(sctp, id);
    if (sctp->send_tsn != tsn) return;
    peer_sctp_stream_t *stream = jetkvm_sctp_get_stream(sctp, id);
    if (stream) stream->send_count--;
}
