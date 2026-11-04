/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEBRTC_HTTP_CANDIDATE_POLICY_NAME "http-peer-ipv4-rewrite-v1"
#define WEBRTC_HTTP_CANDIDATE_MAX 1024U
#define WEBRTC_HTTP_CANDIDATE_REWRITTEN_MAX \
    (WEBRTC_HTTP_CANDIDATE_MAX + 16U)
#define WEBRTC_HTTP_CANDIDATE_LIMIT 16U

typedef struct {
    uint8_t octet[4];
} webrtc_http_ipv4_t;

typedef struct {
    bool active;
    bool owner_bound;
    bool rewrite_enabled;
    uint32_t session_id;
    webrtc_http_ipv4_t owner;
    webrtc_http_ipv4_t device;
    unsigned candidate_count;
    uint16_t candidate_ports[WEBRTC_HTTP_CANDIDATE_LIMIT];
} webrtc_http_candidate_policy_t;

typedef enum {
    WEBRTC_HTTP_OWNER_ALLOW = 0,
    WEBRTC_HTTP_OWNER_PEER_REQUIRED,
    WEBRTC_HTTP_OWNER_PEER_MISMATCH,
} webrtc_http_owner_result_t;

typedef enum {
    WEBRTC_HTTP_CANDIDATE_OK = 0,
    WEBRTC_HTTP_CANDIDATE_POLICY_UNAVAILABLE,
    WEBRTC_HTTP_CANDIDATE_TOO_LONG,
    WEBRTC_HTTP_CANDIDATE_INVALID,
    WEBRTC_HTTP_CANDIDATE_LIMIT_REACHED,
    WEBRTC_HTTP_CANDIDATE_OUTPUT_TOO_SMALL,
} webrtc_http_candidate_result_t;

bool webrtc_http_ipv4_parse(const char *text, webrtc_http_ipv4_t *address);

bool webrtc_http_ipv4_format(const webrtc_http_ipv4_t *address,
                             char *output, size_t output_size);

bool webrtc_http_ipv4_equal(const webrtc_http_ipv4_t *left,
                            const webrtc_http_ipv4_t *right);

/* RFC 1918 or link-local (169.254/16). */
bool webrtc_http_ipv4_private(const webrtc_http_ipv4_t *address);

/* Replaces the previous single HTTP session's ownership atomically. */
void webrtc_http_candidate_policy_begin(
    webrtc_http_candidate_policy_t *policy, uint32_t session_id,
    const webrtc_http_ipv4_t *owner, const webrtc_http_ipv4_t *device);

void webrtc_http_candidate_policy_clear(
    webrtc_http_candidate_policy_t *policy, uint32_t session_id);

/* Stale/unknown sessions are left to the transport's existing 410 handling. */
webrtc_http_owner_result_t webrtc_http_candidate_policy_authorize(
    const webrtc_http_candidate_policy_t *policy, uint32_t session_id,
    const webrtc_http_ipv4_t *request_peer);

/*
 * Validates a browser host candidate and replaces only its address token with
 * the session's kernel-observed HTTP peer IPv4.  The input need not be NUL
 * terminated; output is always NUL terminated on success.
 */
webrtc_http_candidate_result_t webrtc_http_candidate_policy_rewrite(
    webrtc_http_candidate_policy_t *policy, uint32_t session_id,
    const char *candidate, size_t candidate_size,
    char *output, size_t output_size);

/* Apply the same LAN policy to candidates already embedded in an SDP offer.
 * Candidates rejected by the local policy are omitted, preserving other SDP
 * lines. Output capacity errors still fail. Input/output must not overlap. */
webrtc_http_candidate_result_t webrtc_http_candidate_policy_rewrite_sdp(
    webrtc_http_candidate_policy_t *policy, uint32_t session_id,
    const char *sdp, size_t sdp_size, char *output, size_t output_size);

/* For a tailnet signaling socket, advertise the device's tunnel address
 * instead of the LAN address gathered by the peer library. */
bool webrtc_http_candidate_policy_for_device(
    const webrtc_http_candidate_policy_t *policy,
    webrtc_http_candidate_policy_t *device_policy);

typedef enum {
    WEBRTC_HTTP_DEVICE_SIGNAL_SEND = 0,
    WEBRTC_HTTP_DEVICE_SIGNAL_SKIP,
    WEBRTC_HTTP_DEVICE_SIGNAL_FAIL,
} webrtc_http_device_signal_t;

/* Rewrites one device signal for a tailnet socket with the device policy of
 * webrtc_http_candidate_policy_for_device(): an SDP (sdp true) or one trickle
 * candidate, with or without "a=" and a line end. A trickle candidate that the
 * policy does not admit is SKIP, so the caller discards it, as the SDP rewrite
 * omits such a line. FAIL means the output does not fit. */
webrtc_http_device_signal_t webrtc_http_candidate_policy_rewrite_device_signal(
    webrtc_http_candidate_policy_t *device_policy, uint32_t session_id, bool sdp,
    const char *signal, size_t signal_size, char *output, size_t output_size);
