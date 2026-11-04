/* SCTP receive-window and message-reserve policy.
 *
 * The vendor holds every received DATA chunk in one cache until its message
 * is delivered, and frees the cache only from the lowest TSN up. After a
 * lost chunk, the chunks behind it filled the cache, and the vendor refused
 * the lost chunk's retransmission ("No buffer for TSN") for the rest of the
 * session: the cumulative acknowledgement stopped, and every channel with
 * it. Its SACK advertised the cache size less the bytes held, which wrapped
 * to about 4 GiB when the bytes held passed the cache size: a chunk in a
 * reused buffer counted at the buffer's size, not its own. Each chunk now
 * counts at its own size (fit_free_buffer).
 *
 * Dropping a held chunk to make room does not work with Chrome: dcsctp never
 * sends a chunk again once a SACK reported it held. So, as dcsctp's own
 * receiver does above its watermark, chunks after the next one may fill the
 * cache only up to PEER_SCTP_RECEIVE_RESERVE below its size, and SACKs
 * advertise only that window. The next chunk may use the reserve. The
 * fragments of a message have consecutive TSNs (RFC 9260 6.9), so at most
 * one message is partly below the cumulative acknowledgement, and the
 * reserve holds the longest message the browser may send (the SDP's
 * max-message-size): that message always completes, is delivered and frees
 * its buffers.
 *
 * sctp_incoming_data's DATA chunk comes to jetkvm_sctp_receive_data and
 * do_sack's window to jetkvm_sctp_receive_window. Both run on the task that
 * runs esp_peer_main_loop(), as the rest of esp_peer does.
 */
#include "peer_sctp_hid_internal.h"

/* Defined in third_party/esp_peer/src; not in its public headers. */
extern int media_lib_mutex_lock(void *, uint32_t);
extern int media_lib_mutex_unlock(void *);
extern void *media_lib_realloc(void *, size_t);
extern int jetkvm_vendor_packet_hdlr(peer_sctp_t *, const void *chunk, int control);

#define DATA_HEADER 16U

static uint32_t window_size(const peer_sctp_buffers_t *buffers)
{
    return buffers->limit_size - PEER_SCTP_RECEIVE_RESERVE;
}

/* The vendor stores a chunk in the first free buffer whose block is large
 * enough and counts it at the block's size, so short fragments in reused
 * blocks counted many times their size. Shrink that block to the chunk. */
static void fit_free_buffer(peer_sctp_buffers_t *buffers, uint32_t size)
{
    media_lib_mutex_lock(buffers->lock, UINT32_MAX);
    peer_sctp_buffer_t *buffer = buffers->free_list;
    if (buffer && buffer->allocated_size > size) {
        void *data = media_lib_realloc(buffer->data, size);
        if (data || size == 0U) {
            buffer->data = data;
            buffer->allocated_size = size;
        }
    }
    media_lib_mutex_unlock(buffers->lock);
}

/* control: a DCEP message, which the vendor holds as an empty buffer. */
int jetkvm_sctp_receive_data(peer_sctp_t *sctp, const void *chunk, int control)
{
    const uint8_t *wire = chunk;
    const uint32_t tsn = (uint32_t)wire[4] << 24 | (uint32_t)wire[5] << 16 |
                         (uint32_t)wire[6] << 8 | wire[7];
    /* The caller checked the chunk's length against the packet, not against
     * the DATA header. */
    const uint32_t length = (uint32_t)wire[2] << 8 | wire[3];
    peer_sctp_buffers_t *buffers = sctp->receive_buffers;
    if (length < DATA_HEADER || !buffers) return -1;
    const uint32_t size = control ? 0U : length - DATA_HEADER;
    const uint32_t window = window_size(buffers);
    if (peer_sctp_after(tsn, sctp->acknowledged_tsn + 1U) &&
        (buffers->fill_size > window || size > window - buffers->fill_size)) {
        /* As the vendor does for every DATA chunk, so SACKs go on. */
        sctp->sack_count = 0;
        return -1;
    }
    fit_free_buffer(buffers, size);
    return jetkvm_vendor_packet_hdlr(sctp, chunk, control);
}

uint32_t jetkvm_sctp_receive_window(peer_sctp_buffers_t *buffers)
{
    media_lib_mutex_lock(buffers->lock, UINT32_MAX);
    const uint32_t held = buffers->fill_size, window = window_size(buffers);
    media_lib_mutex_unlock(buffers->lock);
    return held < window ? window - held : 0U;
}
