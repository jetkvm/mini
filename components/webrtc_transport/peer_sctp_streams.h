/* Closing one DataChannel by its SCTP stream ID.
 *
 * esp_peer_close_data_channel() names the channel by label, and the vendor
 * resets the first open stream with that label. Several guest channels share
 * one label ("guest-terminal"), and a browser may give any label to any
 * channel, so the transport never closes by a browser's label: it passes the
 * label below, which the SCTP adapter (peer_sctp_reconfig.c) resolves to
 * exactly one stream. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The mark, up to five digits and the terminator. */
#define PEER_SCTP_STREAM_LABEL_CAPACITY 8U
#define PEER_SCTP_STREAM_LABEL_MARK '\x1f'

static inline size_t peer_sctp_stream_label(
    char label[PEER_SCTP_STREAM_LABEL_CAPACITY], uint16_t stream_id)
{
    char digits[5];
    size_t count = 0U;
    do {
        digits[count++] = (char)('0' + stream_id % 10U);
        stream_id = (uint16_t)(stream_id / 10U);
    } while (stream_id != 0U);
    size_t length = 0U;
    label[length++] = PEER_SCTP_STREAM_LABEL_MARK;
    while (count > 0U) label[length++] = digits[--count];
    label[length] = '\0';
    return length;
}

/* Only what peer_sctp_stream_label() makes: the mark, then a stream ID in
 * decimal without leading zeros. */
static inline bool peer_sctp_parse_stream_label(const char *label,
                                                uint16_t *stream_id)
{
    if (!label || label[0] != PEER_SCTP_STREAM_LABEL_MARK ||
        label[1] < '0' || label[1] > '9' ||
        (label[1] == '0' && label[2] != '\0')) {
        return false;
    }
    uint32_t value = 0U;
    size_t i = 1U;
    for (; label[i] >= '0' && label[i] <= '9'; ++i) {
        if (i > 5U) return false;
        value = value * 10U + (uint32_t)(label[i] - '0');
    }
    if (label[i] != '\0' || value > UINT16_MAX) return false;
    *stream_id = (uint16_t)value;
    return true;
}
