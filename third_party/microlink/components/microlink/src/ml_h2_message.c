#include "ml_h2_message.h"
#include <string.h>

bool ml_h2_data_payload(uint8_t flags, const uint8_t **data, size_t *size)
{
    if (!(flags & 8)) return true;
    if (!*size || (size_t)(*data)[0] + 1 > *size) return false;
    *size -= (size_t)(*data)[0] + 1;
    ++*data;
    return true;
}

void ml_h2_stream_reset(ml_h2_stream_t *state)
{
    state->header_length = state->frame_length = state->frame_received = 0;
    state->prefix_length = state->message_length = state->message_received = 0;
}

static bool stream_frame(ml_h2_stream_t *s, ml_h2_frame_callback_t frame_callback,
    ml_h2_message_callback_t message_callback, void *ctx)
{
    uint8_t type = s->header[3], flags = s->header[4];
    uint32_t id = (uint32_t)(s->header[5] & 0x7f) << 24 |
        (uint32_t)s->header[6] << 16 | (uint32_t)s->header[7] << 8 | s->header[8];
    if (type == 7 || (id == s->stream && type == 3)) return false;
    if (frame_callback && !frame_callback(type, flags, id, s->frame, s->frame_length, ctx))
        return false;
    if (id == s->stream && type == 0) {
        const uint8_t *data = s->frame;
        size_t size = s->frame_length;
        if (!ml_h2_data_payload(flags, &data, &size)) return false;
        while (size) {
            if (s->prefix_length < 4) {
                s->prefix[s->prefix_length++] = *data++;
                --size;
                if (s->prefix_length != 4) continue;
                s->message_length = (size_t)s->prefix[0] | (size_t)s->prefix[1] << 8 |
                    (size_t)s->prefix[2] << 16 | (size_t)s->prefix[3] << 24;
                if (!s->message_length || s->message_length >= s->message_capacity) return false;
            }
            size_t take = s->message_length - s->message_received;
            if (take > size) take = size;
            memcpy(s->message + s->message_received, data, take);
            s->message_received += take;
            data += take;
            size -= take;
            if (s->message_received == s->message_length) {
                s->message[s->message_length] = 0;
                if (!message_callback(s->message, s->message_length, ctx)) return false;
                s->prefix_length = s->message_length = s->message_received = 0;
            }
        }
    }
    return !((id == s->stream) && (type == 0 || type == 1) && (flags & 1));
}

bool ml_h2_stream_feed(ml_h2_stream_t *s, const uint8_t *data, size_t length,
    ml_h2_frame_callback_t frame_callback, ml_h2_message_callback_t message_callback, void *ctx)
{
    if (!s || (!data && length) || !s->frame || !s->message || !s->stream || !message_callback)
        return false;
    while (length) {
        if (s->header_length < 9) {
            size_t take = 9 - s->header_length;
            if (take > length) take = length;
            memcpy(s->header + s->header_length, data, take);
            s->header_length += take;
            data += take;
            length -= take;
            if (s->header_length != 9) continue;
            s->frame_length = (size_t)s->header[0] << 16 |
                (size_t)s->header[1] << 8 | s->header[2];
            if (s->frame_length > s->frame_capacity) return false;
        }
        size_t take = s->frame_length - s->frame_received;
        if (take > length) take = length;
        memcpy(s->frame + s->frame_received, data, take);
        s->frame_received += take;
        data += take;
        length -= take;
        if (s->frame_received == s->frame_length) {
            if (!stream_frame(s, frame_callback, message_callback, ctx)) return false;
            s->header_length = s->frame_length = s->frame_received = 0;
        }
    }
    return true;
}

int ml_h2_first_message(const uint8_t *frames, size_t length, uint32_t stream,
                        uint8_t *message, size_t capacity, size_t *message_length)
{
    if (!frames || !message || !message_length || !stream) return -1;
    *message_length = 0;
    uint8_t prefix[4];
    size_t prefix_length = 0, declared = 0, copied = 0;
    for (size_t pos = 0; length - pos >= 9;) {
        size_t size = (size_t)frames[pos] << 16 | (size_t)frames[pos + 1] << 8 | frames[pos + 2];
        uint8_t type = frames[pos + 3], flags = frames[pos + 4];
        uint32_t id = (uint32_t)(frames[pos + 5] & 0x7f) << 24 |
            (uint32_t)frames[pos + 6] << 16 | (uint32_t)frames[pos + 7] << 8 | frames[pos + 8];
        pos += 9;
        if (size > length - pos) return 0;
        const uint8_t *data = frames + pos;
        pos += size;
        if (type == 7 || (id == stream && type == 3)) return -1;
        if (id != stream) continue;
        if (type == 0) {
            if (!ml_h2_data_payload(flags, &data, &size)) return -1;
            while (prefix_length < 4 && size) {
                prefix[prefix_length++] = *data++;
                --size;
                if (prefix_length == 4) {
                    declared = (size_t)prefix[0] | (size_t)prefix[1] << 8 |
                        (size_t)prefix[2] << 16 | (size_t)prefix[3] << 24;
                    if (!declared || declared >= capacity) return -1;
                }
            }
            if (prefix_length == 4) {
                size_t take = size < declared - copied ? size : declared - copied;
                memcpy(message + copied, data, take);
                copied += take;
                if (copied == declared) {
                    message[copied] = 0;
                    *message_length = copied;
                    return 1;
                }
            }
        }
        if ((type == 0 || type == 1) && (flags & 1)) return -1;
    }
    return 0;
}
