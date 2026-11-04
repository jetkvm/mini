/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The DTLS peer of a WebRTC connection is the endpoint whose certificate the
 * remote description names in its a=fingerprint lines (RFC 8122, RFC 8827).
 * esp_peer_send_msg() records them for each remote SDP of a peer connection,
 * and the handshake of that connection accepts only a matching certificate. */

/* Records the sha-256, sha-384 and sha-512 fingerprints of a remote SDP for
 * `peer` in place of earlier ones. Returns false, with none recorded, for an
 * SDP without such a fingerprint. */
bool dtls_fingerprint_expect(const void *peer, const char *sdp, size_t size);

/* Whether a DER certificate has a recorded fingerprint of `peer`. */
bool dtls_fingerprint_matches(const void *peer, const unsigned char *der, size_t size);

void dtls_fingerprint_forget(const void *peer);
