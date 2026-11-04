#include "peer_sctp_hid_internal.h"
#include <string.h>

/* Defined in third_party/esp_peer/src (media_lib_weak.c or the application's
 * media_lib overrides, core/sctp.c, core/buf_mngr.c); not in its public
 * headers. */
extern int media_lib_mutex_lock(void *, uint32_t);
extern int media_lib_mutex_unlock(void *);
extern void media_lib_free(void *);
extern peer_sctp_frame_t *jetkvm_vendor_get_frame(peer_sctp_t *, peer_sctp_stream_t *, const void *);
extern int buf_mngr_abandon(peer_sctp_buffers_t *, uint32_t, uint32_t);
extern void update_receive_ack_tsn(peer_sctp_t *, bool, int);
extern peer_sctp_frame_t *check_send_all_stream(peer_sctp_t *, bool);

peer_sctp_frame_t *jetkvm_sctp_get_frame(peer_sctp_t *sctp,
                                       peer_sctp_stream_t *stream,
                                       const void *chunk)
{
    if (!(stream->flags & 0x40)) {
        /* esp_peer's parser already converted the DATA header to host order.
         * Lookup must precede jetkvm_vendor_get_frame(): its IN_RANGE() and
         * SEQ_LT() compare 16-bit SSNs with 32-bit arithmetic, which is wrong
         * across the 65535-to-0 rollover, so a later fragment can allocate a
         * second frame for the same SSN. */
        uint16_t sequence;
        memcpy(&sequence, (const uint8_t *)chunk + 10, sizeof(sequence));
        media_lib_mutex_lock(sctp->lock, UINT32_MAX);
        for (peer_sctp_frame_t *existing = stream->frames; existing; existing = existing->next) {
            if (existing->sequence == sequence) {
                media_lib_mutex_unlock(sctp->lock);
                return existing;
            }
        }
        media_lib_mutex_unlock(sctp->lock);
    }
    peer_sctp_frame_t *frame = jetkvm_vendor_get_frame(sctp, stream, chunk);
    if (!frame || (stream->flags & 0x40)) return frame; /* unordered */
    media_lib_mutex_lock(sctp->lock, UINT32_MAX);
    /* esp_peer's insertion compares a 32-bit subtraction against 32768.
     * Reinsert this frame with 16-bit serial ordering, including SSN rollover.
     * The remaining list was ordered by the preceding call to this adapter. */
    peer_sctp_frame_t **position = &stream->frames;
    while (*position && *position != frame) position = &(*position)->next;
    if (*position) {
        *position = frame->next;
        position = &stream->frames;
        while (*position && (int16_t)((uint16_t)(*position)->sequence -
                                      (uint16_t)frame->sequence) <= 0) {
            position = &(*position)->next;
        }
        frame->next = *position;
        *position = frame;
        stream->tail = stream->frames;
        while (stream->tail->next) stream->tail = stream->tail->next;
        stream->first_sequence = stream->frames->sequence;
        stream->last_sequence = stream->tail->sequence;
    }
    media_lib_mutex_unlock(sctp->lock);
    return frame;
}

static bool complete_frame_for_buffer(const peer_sctp_t *sctp,
                                      const peer_sctp_buffer_t *buffer)
{
    for (unsigned i = 0; i < sizeof(sctp->streams) / sizeof(sctp->streams[0]); ++i) {
        const peer_sctp_stream_t *stream = &sctp->streams[i];
        if (!(stream->flags & 1) || stream->id != buffer->stream_id) continue;
        for (const peer_sctp_frame_t *frame = stream->frames; frame; frame = frame->next) {
            if ((frame->flags & 3) == 3 && frame->fill_count == 1 &&
                frame->min_tsn == buffer->tsn && frame->max_tsn == buffer->tsn) return true;
        }
    }
    return false;
}

/* buf_mngr_abandon adds an empty buffer for each TSN it skips that never
 * arrived, taking free buffers from the front of the list without counting
 * them in the bytes held; their removal then subtracted the block each kept.
 * Free the blocks of that many free buffers first. */
static void empty_free_blocks(peer_sctp_buffers_t *buffers, uint32_t count)
{
    for (peer_sctp_buffer_t *buffer = buffers->free_list; buffer && count;
         buffer = buffer->next, --count) {
        media_lib_free(buffer->data);
        buffer->data = NULL;
        buffer->allocated_size = 0;
    }
}

void jetkvm_sctp_handle_forward_tsn(peer_sctp_t *sctp, const void *chunk)
{
    const uint8_t *wire = chunk;
    unsigned length = ((unsigned)wire[2] << 8) | wire[3];
    /* The caller checks chunk length against the enclosing SCTP packet. */
    if (length < 8 || (length - 8) % 4 || !sctp->receive_buffers) return;
    uint32_t advanced = (uint32_t)wire[4] << 24 | (uint32_t)wire[5] << 16 |
                        (uint32_t)wire[6] << 8 | wire[7];
    media_lib_mutex_lock(sctp->lock, UINT32_MAX);
    uint32_t previous = sctp->acknowledged_tsn;
    if (!peer_sctp_after(advanced, previous)) {
        media_lib_mutex_unlock(sctp->lock);
        return;
    }
    peer_sctp_buffers_t *buffers = sctp->receive_buffers;
    uint32_t received = 0;
    media_lib_mutex_lock(buffers->lock, UINT32_MAX);
    for (peer_sctp_buffer_t *buffer = buffers->head; buffer; buffer = buffer->next) {
        received += peer_sctp_after(buffer->tsn, previous) &&
                    !peer_sctp_after(buffer->tsn, advanced);
        /* Preserve received, complete single-fragment messages behind a lost
         * TSN. Mouse reports use this path. Incomplete reliable messages already
         * carry the vendor's permanent 0x02 buffer flag and are left untouched.
         * Incomplete partially reliable tails remain eligible for abandonment.
         * Bit 7 is unused padding in this exact vendor layout; tag only the
         * reliability bits temporarily added here, then restore them below. */
        if ((buffer->flags & 7) == 1 && buffer->data && buffer->size &&
            peer_sctp_after(buffer->tsn, previous) && !peer_sctp_after(buffer->tsn, advanced) &&
            complete_frame_for_buffer(sctp, buffer)) buffer->flags |= 0x82;
    }
    empty_free_blocks(buffers, advanced - previous - received);
    media_lib_mutex_unlock(buffers->lock);
    buf_mngr_abandon(buffers, previous, advanced);
    media_lib_mutex_lock(buffers->lock, UINT32_MAX);
    for (peer_sctp_buffer_t *buffer = buffers->head; buffer; buffer = buffer->next) {
        if (buffer->flags & 0x80) buffer->flags &= (uint8_t)~0x82;
    }
    media_lib_mutex_unlock(buffers->lock);
    sctp->acknowledged_tsn = advanced;
    media_lib_mutex_unlock(sctp->lock);
    /* This vendor's cumulative drain has no "next SSN" state to advance:
     * check_send_all_stream selects frames by min/max TSN, and send_all_frames
     * checks fragment completeness. Its separate early-delivery SSN gate is
     * enabled only after reliable delivery (stream flags 0x20/0x80), so it does
     * not block this partially reliable movement path. The FORWARD-TSN SID/SSN
     * entries therefore do not need to update a stream cursor here. */
    update_receive_ack_tsn(sctp, true, 0);
    /* A FORWARD-TSN can itself reach the last buffered message. In that case
     * update_receive_ack_tsn finds no further advance and skips its drain. */
    check_send_all_stream(sctp, true);
}
