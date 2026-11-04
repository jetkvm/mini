#include "ml_derp_stream.h"

#include <stdlib.h>
#include <string.h>

void ml_derp_stream_reset(ml_derp_stream_t *s) {
    free(s->rx_data);
    free(s->tx_data);
    memset(s, 0, sizeof(*s));
}

int ml_derp_stream_begin(ml_derp_stream_t *s, uint8_t type,
                         const uint8_t *peer_key, const uint8_t *data,
                         size_t len, uint64_t now_ms,
                         ml_derp_stream_alloc_fn allocate) {
    const ml_derp_stream_write_frame_t frame = { type, peer_key, data, len };
    return ml_derp_stream_begin_batch(s, &frame, 1, now_ms, allocate);
}

int ml_derp_stream_begin_batch(ml_derp_stream_t *s,
                               const ml_derp_stream_write_frame_t *frames,
                               size_t count, uint64_t now_ms,
                               ml_derp_stream_alloc_fn allocate) {
    if (s->tx_data || !frames || !count) return ML_DERP_STREAM_FAIL;
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t prefix = frames[i].peer_key ? 32 : 0;
        if (frames[i].len > ML_DERP_STREAM_MAX_PAYLOAD - prefix ||
            (frames[i].len && !frames[i].data)) return ML_DERP_STREAM_FAIL;
        size_t len = 5 + prefix + frames[i].len;
        if (total > SIZE_MAX - len) return ML_DERP_STREAM_FAIL;
        total += len;
    }
    /* Runtime batches fit one TLS record. A single frame can use the normal
     * protocol limit and is sent through partial writes if necessary. */
    if (count > 1 && total > 16384) return ML_DERP_STREAM_FAIL;
    uint8_t *buffer = allocate(total);
    if (!buffer) return ML_DERP_STREAM_FAIL;
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        const ml_derp_stream_write_frame_t *frame = &frames[i];
        size_t prefix = frame->peer_key ? 32 : 0;
        size_t payload_len = prefix + frame->len;
        uint8_t *out = buffer + offset;
        out[0] = frame->type;
        out[1] = (payload_len >> 24) & 0xff;
        out[2] = (payload_len >> 16) & 0xff;
        out[3] = (payload_len >> 8) & 0xff;
        out[4] = payload_len & 0xff;
        if (prefix) memcpy(out + 5, frame->peer_key, prefix);
        if (frame->len) memcpy(out + 5 + prefix, frame->data, frame->len);
        offset += 5 + payload_len;
    }
    s->tx_data = buffer;
    s->tx_len = total;
    s->tx_frames = count;
    s->tx_used = 0;
    s->tx_progress_ms = now_ms;
    return 1;
}

int ml_derp_stream_write(ml_derp_stream_t *s, ml_derp_stream_io_fn write,
                         void *context, uint64_t now_ms) {
    if (!s->tx_data) return 0;
    if (now_ms - s->tx_progress_ms > ML_DERP_STREAM_IDLE_TIMEOUT_MS)
        return ML_DERP_STREAM_FAIL;
    int n = write(context, s->tx_data + s->tx_used, s->tx_len - s->tx_used);
    if (n == ML_DERP_STREAM_AGAIN) return 0;
    if (n <= 0 || (size_t)n > s->tx_len - s->tx_used)
        return ML_DERP_STREAM_FAIL;
    s->tx_used += n;
    s->tx_progress_ms = now_ms;
    if (s->tx_used == s->tx_len) {
        free(s->tx_data);
        s->tx_data = NULL;
        s->tx_len = s->tx_used = 0;
    }
    return 1;
}

int ml_derp_stream_read(ml_derp_stream_t *s, ml_derp_stream_io_fn read,
                        void *context, uint64_t now_ms,
                        ml_derp_stream_alloc_fn allocate,
                        ml_derp_stream_frame_t *frame) {
    if (s->header_used && now_ms - s->rx_progress_ms > ML_DERP_STREAM_IDLE_TIMEOUT_MS)
        return ML_DERP_STREAM_FAIL;
    if (s->header_used < sizeof(s->header)) {
        int n = read(context, s->header + s->header_used,
                     sizeof(s->header) - s->header_used);
        if (n == ML_DERP_STREAM_AGAIN) return 0;
        if (n <= 0 || (size_t)n > sizeof(s->header) - s->header_used)
            return ML_DERP_STREAM_FAIL;
        s->header_used += n;
        s->rx_progress_ms = now_ms;
        if (s->header_used < sizeof(s->header)) return 0;
        s->rx_len = ((uint32_t)s->header[1] << 24) |
                    ((uint32_t)s->header[2] << 16) |
                    ((uint32_t)s->header[3] << 8) | s->header[4];
        if (s->rx_len > ML_DERP_STREAM_MAX_PAYLOAD) return ML_DERP_STREAM_FAIL;
        if (s->rx_len) {
            s->rx_data = allocate(s->rx_len);
            if (!s->rx_data) return ML_DERP_STREAM_FAIL;
        }
    }
    if (s->rx_used < s->rx_len) {
        int n = read(context, s->rx_data + s->rx_used, s->rx_len - s->rx_used);
        if (n == ML_DERP_STREAM_AGAIN) return 0;
        if (n <= 0 || (size_t)n > s->rx_len - s->rx_used)
            return ML_DERP_STREAM_FAIL;
        s->rx_used += n;
        s->rx_progress_ms = now_ms;
        if (s->rx_used < s->rx_len) return 0;
    }
    *frame = (ml_derp_stream_frame_t){ s->header[0], s->rx_data, s->rx_len };
    s->rx_data = NULL;
    s->header_used = s->rx_len = s->rx_used = 0;
    return 1;
}
