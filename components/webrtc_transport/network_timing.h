/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "webrtc_transport.h"

typedef enum {
    WEBRTC_NETWORK_STAGE_SRTP = 0,
    WEBRTC_NETWORK_STAGE_LWIP_SENDTO,
    WEBRTC_NETWORK_STAGE_EMAC_HANDOFF,
} webrtc_network_stage_t;

void webrtc_network_timing_wrappers_link_anchor(void);
void webrtc_network_timing_reset(uint32_t epoch);
void webrtc_network_timing_begin(uint32_t epoch, uint32_t rtp_timestamp,
                                 int64_t capture_eof_us);
void webrtc_network_timing_cancel(uint32_t epoch, uint32_t rtp_timestamp,
                                  int64_t capture_eof_us);
/* The SSRC whose resent packets count as rtp_retransmits. */
void webrtc_network_timing_set_video_ssrc(uint32_t ssrc);
void webrtc_network_timing_record_rtp(webrtc_network_stage_t stage,
                                      const uint8_t *rtp, size_t rtp_size,
                                      int64_t started_us, int64_t completed_us,
                                      bool success);
void webrtc_network_timing_record_srtp_error(
    int status, const uint8_t *rtp, size_t rtp_size,
    size_t output_capacity, size_t output_size);
void webrtc_network_timing_record_srtp_breakdown(
    const uint8_t *rtp, size_t rtp_size, uint32_t cipher_us,
    uint32_t auth_and_framework_us);
void webrtc_network_timing_record_ethernet(
    const uint8_t *frame, size_t frame_size, int64_t started_us,
    int64_t completed_us, bool success);
void webrtc_network_timing_snapshot(
    webrtc_transport_network_timing_stats_t *network,
    webrtc_transport_stage_age_summary_t *stage_age);
