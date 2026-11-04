#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RFC 4585 PLI, including reduced-size or compound RTCP. The caller must
 * authenticate/decrypt SRTCP before passing the plaintext here. */
bool webrtc_rtcp_has_video_pli(const uint8_t *data, size_t size,
                               uint32_t video_ssrc);

/* RFC 3550 receiver report block for one source, from an SR or RR. */
typedef struct {
    uint8_t fraction_lost;     /* lost/expected since the previous report, 1/256 */
    int32_t cumulative_lost;   /* signed 24-bit total */
    uint32_t highest_sequence; /* extended highest sequence number received */
} webrtc_rtcp_report_block_t;

/* Finds the report block for video_ssrc in plaintext (compound) RTCP.
 * Returns false when the packet is malformed or has no such block. */
bool webrtc_rtcp_find_report_block(const uint8_t *data, size_t size,
                                   uint32_t video_ssrc,
                                   webrtc_rtcp_report_block_t *block);
