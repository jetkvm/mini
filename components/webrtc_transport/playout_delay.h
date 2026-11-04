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

#define WEBRTC_PLAYOUT_DELAY_URI \
    "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay"
#define WEBRTC_PLAYOUT_DELAY_PREFERRED_EXT_ID 6
#define WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD 8

typedef void *(*webrtc_playout_delay_alloc_fn)(size_t size);
typedef void (*webrtc_playout_delay_free_fn)(void *memory);

typedef struct {
    webrtc_playout_delay_alloc_fn alloc;
    webrtc_playout_delay_free_fn free;
} webrtc_playout_delay_allocator_t;

typedef struct {
    char *data;
    size_t size;
    uint8_t extension_id;
    bool inserted;
    /* Bound to the allocator used by prepare_* so release remains correct
     * even when the result crosses helper/callback boundaries. */
    webrtc_playout_delay_free_fn free_fn;
} webrtc_playout_delay_offer_t;

/* The owned SDP result has the same lifetime contract for an answer. */
typedef webrtc_playout_delay_offer_t webrtc_playout_delay_answer_t;

typedef enum {
    WEBRTC_PLAYOUT_DELAY_RTP_INVALID = -1,
    WEBRTC_PLAYOUT_DELAY_RTP_BYPASS_EXISTING_EXTENSION = 0,
    WEBRTC_PLAYOUT_DELAY_RTP_STAMPED = 1,
} webrtc_playout_delay_rtp_result_t;

/*
 * Copy an SDP offer and ensure that its video media section advertises the
 * playout-delay extension.  The caller owns result->data on success.
 */
int webrtc_playout_delay_prepare_offer(const uint8_t *sdp, size_t size,
                                       const webrtc_playout_delay_allocator_t *allocator,
                                       webrtc_playout_delay_offer_t *result);
void webrtc_playout_delay_release_offer(webrtc_playout_delay_offer_t *offer);

/* True only when the video media section accepted the offered extension ID. */
bool webrtc_playout_delay_answer_accepts(const char *sdp, size_t size,
                                         uint8_t extension_id);

/*
 * Inspect a browser-created offer for a video playout-delay extmap that the
 * browser is willing to receive.  Controlled/answerer sessions must never
 * invent an extension ID: only the exact offered ID may be returned.
 */
bool webrtc_playout_delay_offer_accepts_sender(const char *sdp, size_t size,
                                               uint8_t *extension_id);

/*
 * Copy a locally generated send-video answer and reflect the exact extension
 * ID from the remote offer.  An existing conflicting ID/direction is rejected
 * instead of producing an SDP that disagrees with the RTP wire format.
 */
int webrtc_playout_delay_prepare_answer(
    const uint8_t *sdp, size_t size, uint8_t offered_extension_id,
    const webrtc_playout_delay_allocator_t *allocator,
    webrtc_playout_delay_answer_t *result);
void webrtc_playout_delay_release_answer(
    webrtc_playout_delay_answer_t *answer);

/*
 * Plan and write an RFC 8285 one-byte RTP header extension containing the
 * three-byte playout-delay value MIN=0, MAX=0.  The pinned esp_peer sender
 * currently emits RTP without header extensions.  A packet that already has
 * an extension is deliberately left unchanged and counted by the caller.
 */
webrtc_playout_delay_rtp_result_t webrtc_playout_delay_rtp_plan(
    const uint8_t *packet, size_t size, uint8_t extension_id,
    size_t *output_size);
webrtc_playout_delay_rtp_result_t webrtc_playout_delay_rtp_write(
    const uint8_t *packet, size_t size, uint8_t extension_id,
    uint8_t *output, size_t output_capacity, size_t *output_size);

#ifdef __cplusplus
}
#endif
