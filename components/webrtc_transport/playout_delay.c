/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "playout_delay.h"

#include <stdio.h>
#include <string.h>

#define RTP_HEADER_MIN_SIZE 12
#define RTP_EXTENSION_PROFILE_ONE_BYTE 0xBEDE

typedef struct {
    size_t start;
    size_t content_end;
    size_t next;
} sdp_line_t;

typedef enum {
    SDP_DIRECTION_UNSPECIFIED = 0,
    SDP_DIRECTION_SENDONLY,
    SDP_DIRECTION_RECVONLY,
    SDP_DIRECTION_SENDRECV,
    SDP_DIRECTION_INACTIVE,
    SDP_DIRECTION_UNKNOWN,
} sdp_direction_t;

typedef struct {
    bool video_found;
    size_t insert_at;
    bool use_crlf;
    bool used_ids[15];
    uint8_t uri_id;
    uint8_t uri_count;
    sdp_direction_t uri_direction;
    sdp_direction_t media_direction;
} video_extmap_scan_t;

static bool next_line(const uint8_t *data, size_t size, size_t *cursor,
                      sdp_line_t *line)
{
    if (!data || !cursor || !line || *cursor >= size) {
        return false;
    }
    size_t start = *cursor;
    size_t end = start;
    while (end < size && data[end] != '\n') {
        end++;
    }
    size_t content_end = end;
    if (content_end > start && data[content_end - 1] == '\r') {
        content_end--;
    }
    size_t next = end < size ? end + 1 : end;
    *line = (sdp_line_t) {
        .start = start,
        .content_end = content_end,
        .next = next,
    };
    *cursor = next;
    return true;
}

static bool line_starts_with(const uint8_t *data, const sdp_line_t *line,
                             const char *prefix)
{
    size_t prefix_size = strlen(prefix);
    return line->content_end - line->start >= prefix_size &&
           memcmp(data + line->start, prefix, prefix_size) == 0;
}

static bool line_equals(const uint8_t *data, const sdp_line_t *line,
                        const char *value)
{
    size_t value_size = strlen(value);
    return line->content_end - line->start == value_size &&
           memcmp(data + line->start, value, value_size) == 0;
}

static sdp_direction_t parse_direction(const uint8_t *data, size_t start,
                                       size_t end)
{
    size_t size = end - start;
    if (size == 8 && memcmp(data + start, "sendonly", size) == 0) {
        return SDP_DIRECTION_SENDONLY;
    }
    if (size == 8 && memcmp(data + start, "recvonly", size) == 0) {
        return SDP_DIRECTION_RECVONLY;
    }
    if (size == 8 && memcmp(data + start, "sendrecv", size) == 0) {
        return SDP_DIRECTION_SENDRECV;
    }
    if (size == 8 && memcmp(data + start, "inactive", size) == 0) {
        return SDP_DIRECTION_INACTIVE;
    }
    return SDP_DIRECTION_UNKNOWN;
}

static uint8_t parse_extmap_line(const uint8_t *data, const sdp_line_t *line,
                                 bool *uri_matches,
                                 sdp_direction_t *direction)
{
    static const char prefix[] = "a=extmap:";
    *uri_matches = false;
    *direction = SDP_DIRECTION_UNSPECIFIED;
    if (!line_starts_with(data, line, prefix)) {
        return 0;
    }
    size_t cursor = line->start + sizeof(prefix) - 1;
    unsigned value = 0;
    size_t digits = 0;
    while (cursor < line->content_end && data[cursor] >= '0' &&
           data[cursor] <= '9') {
        value = value * 10U + (unsigned)(data[cursor] - '0');
        if (value > UINT8_MAX) {
            return 0;
        }
        cursor++;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    if (cursor < line->content_end && data[cursor] == '/') {
        size_t direction_start = ++cursor;
        while (cursor < line->content_end && data[cursor] != ' ' &&
               data[cursor] != '\t') {
            cursor++;
        }
        *direction = parse_direction(data, direction_start, cursor);
    }
    while (cursor < line->content_end &&
           (data[cursor] == ' ' || data[cursor] == '\t')) {
        cursor++;
    }
    size_t uri_size = strlen(WEBRTC_PLAYOUT_DELAY_URI);
    if (line->content_end - cursor >= uri_size &&
        memcmp(data + cursor, WEBRTC_PLAYOUT_DELAY_URI, uri_size) == 0 &&
        (cursor + uri_size == line->content_end ||
         data[cursor + uri_size] == ' ' || data[cursor + uri_size] == '\t')) {
        *uri_matches = true;
    }
    return (uint8_t)value;
}

static bool scan_video_extmaps(const uint8_t *sdp, size_t size,
                               video_extmap_scan_t *scan)
{
    if (!sdp || !size || !scan) {
        return false;
    }
    memset(scan, 0, sizeof(*scan));
    scan->insert_at = SIZE_MAX;
    bool in_video = false;
    size_t cursor = 0;
    sdp_line_t line = { 0 };
    while (next_line(sdp, size, &cursor, &line)) {
        if (line_starts_with(sdp, &line, "m=")) {
            if (in_video) {
                if (scan->insert_at == SIZE_MAX) {
                    scan->insert_at = line.start;
                }
                break;
            }
            if (line_starts_with(sdp, &line, "m=video ")) {
                in_video = true;
                scan->video_found = true;
                if (line.next == line.content_end) {
                    /* A generated SDP media line must have a delimiter so an
                     * attribute can be inserted without changing that line. */
                    return false;
                }
                scan->use_crlf = line.content_end < size &&
                                 sdp[line.content_end] == '\r';
            }
            continue;
        }
        if (!in_video) {
            continue;
        }
        if (scan->insert_at == SIZE_MAX &&
            line_starts_with(sdp, &line, "a=")) {
            /* RFC 4566 orders optional i=/c=/b=/k= fields before a=. */
            scan->insert_at = line.start;
        }
        if (line_equals(sdp, &line, "a=sendonly")) {
            scan->media_direction = SDP_DIRECTION_SENDONLY;
        } else if (line_equals(sdp, &line, "a=recvonly")) {
            scan->media_direction = SDP_DIRECTION_RECVONLY;
        } else if (line_equals(sdp, &line, "a=sendrecv")) {
            scan->media_direction = SDP_DIRECTION_SENDRECV;
        } else if (line_equals(sdp, &line, "a=inactive")) {
            scan->media_direction = SDP_DIRECTION_INACTIVE;
        }
        bool uri_matches = false;
        sdp_direction_t direction = SDP_DIRECTION_UNSPECIFIED;
        uint8_t id = parse_extmap_line(
            sdp, &line, &uri_matches, &direction);
        if (id >= 1 && id <= 14) {
            scan->used_ids[id] = true;
            if (uri_matches) {
                scan->uri_id = id;
                if (scan->uri_count != UINT8_MAX) {
                    scan->uri_count++;
                }
                scan->uri_direction = direction;
            }
        }
    }
    if (scan->video_found && scan->insert_at == SIZE_MAX) {
        scan->insert_at = size;
    }
    return scan->video_found;
}

static bool direction_allows_sender(sdp_direction_t direction)
{
    return direction == SDP_DIRECTION_UNSPECIFIED ||
           direction == SDP_DIRECTION_SENDONLY ||
           direction == SDP_DIRECTION_SENDRECV;
}

static bool direction_allows_receiver(sdp_direction_t direction)
{
    return direction == SDP_DIRECTION_UNSPECIFIED ||
           direction == SDP_DIRECTION_RECVONLY ||
           direction == SDP_DIRECTION_SENDRECV;
}

int webrtc_playout_delay_prepare_offer(const uint8_t *sdp, size_t size,
                                       const webrtc_playout_delay_allocator_t *allocator,
                                       webrtc_playout_delay_offer_t *result)
{
    if (!sdp || !size || !allocator || !allocator->alloc ||
        !allocator->free || !result) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    video_extmap_scan_t scan = { 0 };
    if (!scan_video_extmaps(sdp, size, &scan)) {
        return -1;
    }
    if (!direction_allows_sender(scan.media_direction)) {
        return -1;
    }
    if (scan.uri_id != 0 && !direction_allows_sender(scan.uri_direction)) {
        return -1;
    }

    uint8_t id = scan.uri_id;
    if (id == 0 && !scan.used_ids[WEBRTC_PLAYOUT_DELAY_PREFERRED_EXT_ID]) {
        id = WEBRTC_PLAYOUT_DELAY_PREFERRED_EXT_ID;
    }
    if (id == 0) {
        for (uint8_t candidate = 1; candidate <= 14; ++candidate) {
            if (!scan.used_ids[candidate]) {
                id = candidate;
                break;
            }
        }
    }
    if (id == 0) {
        return -1;
    }

    if (scan.uri_id != 0) {
        result->data = allocator->alloc(size + 1);
        if (!result->data) {
            return -2;
        }
        memcpy(result->data, sdp, size);
        result->data[size] = '\0';
        result->size = size;
        result->extension_id = id;
        result->inserted = false;
        result->free_fn = allocator->free;
        return 0;
    }

    char attribute[128];
    int written = snprintf(attribute, sizeof(attribute), "a=extmap:%u %s%s",
                           id, WEBRTC_PLAYOUT_DELAY_URI,
                           scan.use_crlf ? "\r\n" : "\n");
    if (written <= 0 || (size_t)written >= sizeof(attribute) ||
        size > SIZE_MAX - (size_t)written - 1) {
        return -1;
    }
    size_t attribute_size = (size_t)written;
    char *copy = allocator->alloc(size + attribute_size + 1);
    if (!copy) {
        return -2;
    }
    memcpy(copy, sdp, scan.insert_at);
    memcpy(copy + scan.insert_at, attribute, attribute_size);
    memcpy(copy + scan.insert_at + attribute_size, sdp + scan.insert_at,
           size - scan.insert_at);
    copy[size + attribute_size] = '\0';
    result->data = copy;
    result->size = size + attribute_size;
    result->extension_id = id;
    result->inserted = true;
    result->free_fn = allocator->free;
    return 0;
}

void webrtc_playout_delay_release_offer(webrtc_playout_delay_offer_t *offer)
{
    if (!offer) {
        return;
    }
    if (offer->data && offer->free_fn) {
        offer->free_fn(offer->data);
    }
    memset(offer, 0, sizeof(*offer));
}

bool webrtc_playout_delay_answer_accepts(const char *sdp, size_t size,
                                         uint8_t extension_id)
{
    if (extension_id < 1 || extension_id > 14) {
        return false;
    }
    video_extmap_scan_t scan = { 0 };
    return scan_video_extmaps((const uint8_t *)sdp, size, &scan) &&
           scan.uri_id == extension_id &&
           direction_allows_receiver(scan.uri_direction) &&
           direction_allows_receiver(scan.media_direction);
}

bool webrtc_playout_delay_offer_accepts_sender(const char *sdp, size_t size,
                                               uint8_t *extension_id)
{
    if (!extension_id) {
        return false;
    }
    *extension_id = 0;
    video_extmap_scan_t scan = { 0 };
    if (!scan_video_extmaps((const uint8_t *)sdp, size, &scan) ||
        scan.uri_count != 1 || scan.uri_id < 1 || scan.uri_id > 14 ||
        !direction_allows_receiver(scan.uri_direction) ||
        !direction_allows_receiver(scan.media_direction)) {
        return false;
    }
    *extension_id = scan.uri_id;
    return true;
}

int webrtc_playout_delay_prepare_answer(
    const uint8_t *sdp, size_t size, uint8_t offered_extension_id,
    const webrtc_playout_delay_allocator_t *allocator,
    webrtc_playout_delay_answer_t *result)
{
    if (!sdp || !size || !result || offered_extension_id < 1 ||
        offered_extension_id > 14 || !allocator || !allocator->alloc ||
        !allocator->free) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    video_extmap_scan_t scan = { 0 };
    if (!scan_video_extmaps(sdp, size, &scan) ||
        !direction_allows_sender(scan.media_direction) ||
        scan.uri_count > 1) {
        return -1;
    }

    if (scan.uri_count == 1) {
        if (scan.uri_id != offered_extension_id ||
            !direction_allows_sender(scan.uri_direction)) {
            return -1;
        }
        result->data = allocator->alloc(size + 1);
        if (!result->data) {
            return -2;
        }
        memcpy(result->data, sdp, size);
        result->data[size] = '\0';
        result->size = size;
        result->extension_id = offered_extension_id;
        result->inserted = false;
        result->free_fn = allocator->free;
        return 0;
    }

    /* The answer may not remap an ID that is already used for another URI. */
    if (scan.used_ids[offered_extension_id]) {
        return -1;
    }
    char attribute[144];
    int written = snprintf(attribute, sizeof(attribute),
                           "a=extmap:%u/sendonly %s%s",
                           offered_extension_id, WEBRTC_PLAYOUT_DELAY_URI,
                           scan.use_crlf ? "\r\n" : "\n");
    if (written <= 0 || (size_t)written >= sizeof(attribute) ||
        size > SIZE_MAX - (size_t)written - 1) {
        return -1;
    }
    size_t attribute_size = (size_t)written;
    char *copy = allocator->alloc(size + attribute_size + 1);
    if (!copy) {
        return -2;
    }
    memcpy(copy, sdp, scan.insert_at);
    memcpy(copy + scan.insert_at, attribute, attribute_size);
    memcpy(copy + scan.insert_at + attribute_size, sdp + scan.insert_at,
           size - scan.insert_at);
    copy[size + attribute_size] = '\0';
    result->data = copy;
    result->size = size + attribute_size;
    result->extension_id = offered_extension_id;
    result->inserted = true;
    result->free_fn = allocator->free;
    return 0;
}

void webrtc_playout_delay_release_answer(
    webrtc_playout_delay_answer_t *answer)
{
    webrtc_playout_delay_release_offer(answer);
}

static bool rtp_payload_offset(const uint8_t *packet, size_t size,
                               size_t *offset)
{
    if (!packet || size < RTP_HEADER_MIN_SIZE || !offset ||
        (packet[0] >> 6) != 2) {
        return false;
    }
    size_t base = RTP_HEADER_MIN_SIZE + (size_t)(packet[0] & 0x0fU) * 4U;
    if (base > size) {
        return false;
    }
    *offset = base;
    return true;
}

webrtc_playout_delay_rtp_result_t webrtc_playout_delay_rtp_plan(
    const uint8_t *packet, size_t size, uint8_t extension_id,
    size_t *output_size)
{
    size_t payload_offset = 0;
    if (!output_size || extension_id < 1 || extension_id > 14 ||
        !rtp_payload_offset(packet, size, &payload_offset)) {
        return WEBRTC_PLAYOUT_DELAY_RTP_INVALID;
    }
    (void)payload_offset;
    if ((packet[0] & 0x10U) != 0) {
        *output_size = size;
        return WEBRTC_PLAYOUT_DELAY_RTP_BYPASS_EXISTING_EXTENSION;
    }
    if (size > SIZE_MAX - WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD) {
        return WEBRTC_PLAYOUT_DELAY_RTP_INVALID;
    }
    *output_size = size + WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD;
    return WEBRTC_PLAYOUT_DELAY_RTP_STAMPED;
}

webrtc_playout_delay_rtp_result_t webrtc_playout_delay_rtp_write(
    const uint8_t *packet, size_t size, uint8_t extension_id,
    uint8_t *output, size_t output_capacity, size_t *output_size)
{
    size_t planned_size = 0;
    webrtc_playout_delay_rtp_result_t plan = webrtc_playout_delay_rtp_plan(
        packet, size, extension_id, &planned_size);
    if (plan != WEBRTC_PLAYOUT_DELAY_RTP_STAMPED) {
        if (output_size) {
            *output_size = size;
        }
        return plan;
    }
    size_t payload_offset = 0;
    if (!output || !output_size || output_capacity < planned_size ||
        !rtp_payload_offset(packet, size, &payload_offset)) {
        return WEBRTC_PLAYOUT_DELAY_RTP_INVALID;
    }

    memcpy(output, packet, payload_offset);
    output[0] |= 0x10U;
    output[payload_offset] = (uint8_t)(RTP_EXTENSION_PROFILE_ONE_BYTE >> 8);
    output[payload_offset + 1] = (uint8_t)RTP_EXTENSION_PROFILE_ONE_BYTE;
    output[payload_offset + 2] = 0;
    output[payload_offset + 3] = 1; /* one 32-bit extension word */
    output[payload_offset + 4] = (uint8_t)((extension_id << 4) | 2U);
    output[payload_offset + 5] = 0; /* MIN high/middle bits */
    output[payload_offset + 6] = 0; /* MIN low/MAX high bits */
    output[payload_offset + 7] = 0; /* MAX low bits */
    memcpy(output + payload_offset + WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD,
           packet + payload_offset, size - payload_offset);
    *output_size = planned_size;
    return WEBRTC_PLAYOUT_DELAY_RTP_STAMPED;
}
