/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEBRTC_H264_OFFER_MAX_PAYLOADS 16
#define WEBRTC_H264_PROFILE_LEVEL_ID_LEN 6

/* The profile-level-id of each video payload type in a browser's offer. */
typedef struct {
    uint8_t count;
    struct {
        uint8_t payload_type;
        char profile_level_id[WEBRTC_H264_PROFILE_LEVEL_ID_LEN];
    } payloads[WEBRTC_H264_OFFER_MAX_PAYLOADS];
} webrtc_h264_offer_profiles_t;

/* Records the profile-level-id of each a=fmtp line in the first video
 * section of an offer.  Payload types past the table size are ignored. */
void webrtc_h264_offer_profiles_parse(const char *sdp, size_t size,
                                      webrtc_h264_offer_profiles_t *profiles);

/*
 * esp_peer can answer an H.264 payload type with profile-level-id 4d001f
 * (Main) although the offer gave another profile for it.  Firefox rejects
 * an answer whose H.264 profile is not the one it offered for that payload
 * type.  This writes the offered profile-level-id into the answer's video
 * a=fmtp lines, in place: the length of the SDP does not change.  Returns
 * true when the answer changed.
 */
bool webrtc_h264_answer_use_offer_profiles(
    char *sdp, size_t size, const webrtc_h264_offer_profiles_t *profiles);

#ifdef __cplusplus
}
#endif
