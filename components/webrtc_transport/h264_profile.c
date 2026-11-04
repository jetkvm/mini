/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "h264_profile.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define PROFILE_KEY "profile-level-id="

typedef struct {
    size_t start;
    size_t end; /* Excludes the line ending. */
} sdp_line_t;

static bool next_line(const char *sdp, size_t size, size_t *cursor,
                      sdp_line_t *line)
{
    if (*cursor >= size) {
        return false;
    }
    line->start = *cursor;
    size_t i = *cursor;
    while (i < size && sdp[i] != '\n') {
        i++;
    }
    *cursor = i < size ? i + 1 : size;
    line->end = (i > line->start && sdp[i - 1] == '\r') ? i - 1 : i;
    return true;
}

static bool starts_with(const char *sdp, const sdp_line_t *line,
                        const char *prefix)
{
    size_t length = strlen(prefix);
    return line->end - line->start >= length &&
           memcmp(sdp + line->start, prefix, length) == 0;
}

/* Parses "a=fmtp:<pt> ...profile-level-id=<6 hex>...".  On success returns
 * the payload type and the offset of the six hex digits. */
static bool parse_fmtp_profile(const char *sdp, const sdp_line_t *line,
                               uint8_t *payload_type, size_t *profile_at)
{
    if (!starts_with(sdp, line, "a=fmtp:")) {
        return false;
    }
    size_t i = line->start + strlen("a=fmtp:");
    unsigned value = 0;
    size_t digits = 0;
    while (i < line->end && isdigit((unsigned char)sdp[i]) && digits < 4) {
        value = value * 10U + (unsigned)(sdp[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || value > 127U || i >= line->end || sdp[i] != ' ') {
        return false;
    }
    const size_t key_length = strlen(PROFILE_KEY);
    for (; i + key_length + WEBRTC_H264_PROFILE_LEVEL_ID_LEN <= line->end; i++) {
        if ((sdp[i - 1] != ' ' && sdp[i - 1] != ';') ||
            memcmp(sdp + i, PROFILE_KEY, key_length) != 0) {
            continue;
        }
        size_t at = i + key_length;
        for (size_t k = 0; k < WEBRTC_H264_PROFILE_LEVEL_ID_LEN; k++) {
            if (!isxdigit((unsigned char)sdp[at + k])) {
                return false;
            }
        }
        size_t after = at + WEBRTC_H264_PROFILE_LEVEL_ID_LEN;
        if (after < line->end && sdp[after] != ';') {
            return false;
        }
        *payload_type = (uint8_t)value;
        *profile_at = at;
        return true;
    }
    return false;
}

/* Steps through the lines of the first video section only. */
typedef struct {
    size_t cursor;
    bool in_video;
    bool done;
} video_lines_t;

static bool next_video_line(const char *sdp, size_t size, video_lines_t *it,
                            sdp_line_t *line)
{
    while (!it->done && next_line(sdp, size, &it->cursor, line)) {
        if (!starts_with(sdp, line, "m=")) {
            if (it->in_video) {
                return true;
            }
            continue;
        }
        it->done = it->in_video;
        it->in_video = starts_with(sdp, line, "m=video ");
    }
    return false;
}

void webrtc_h264_offer_profiles_parse(const char *sdp, size_t size,
                                      webrtc_h264_offer_profiles_t *profiles)
{
    if (!profiles) {
        return;
    }
    memset(profiles, 0, sizeof(*profiles));
    if (!sdp) {
        return;
    }
    video_lines_t it = {0};
    sdp_line_t line;
    while (next_video_line(sdp, size, &it, &line)) {
        uint8_t payload_type = 0;
        size_t at = 0;
        if (profiles->count < WEBRTC_H264_OFFER_MAX_PAYLOADS &&
            parse_fmtp_profile(sdp, &line, &payload_type, &at)) {
            profiles->payloads[profiles->count].payload_type = payload_type;
            memcpy(profiles->payloads[profiles->count].profile_level_id,
                   sdp + at, WEBRTC_H264_PROFILE_LEVEL_ID_LEN);
            profiles->count++;
        }
    }
}

bool webrtc_h264_answer_use_offer_profiles(
    char *sdp, size_t size, const webrtc_h264_offer_profiles_t *profiles)
{
    if (!sdp || !profiles) {
        return false;
    }
    bool changed = false;
    video_lines_t it = {0};
    sdp_line_t line;
    while (next_video_line(sdp, size, &it, &line)) {
        uint8_t payload_type = 0;
        size_t at = 0;
        if (!parse_fmtp_profile(sdp, &line, &payload_type, &at)) {
            continue;
        }
        for (uint8_t i = 0; i < profiles->count; i++) {
            const char *offered = profiles->payloads[i].profile_level_id;
            if (profiles->payloads[i].payload_type != payload_type ||
                strncasecmp(sdp + at, offered,
                            WEBRTC_H264_PROFILE_LEVEL_ID_LEN) == 0) {
                continue;
            }
            memcpy(sdp + at, offered, WEBRTC_H264_PROFILE_LEVEL_ID_LEN);
            changed = true;
            break;
        }
    }
    return changed;
}
