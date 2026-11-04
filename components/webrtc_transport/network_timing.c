/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "network_timing.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "srtp.h"

#define NETWORK_TRACE_SLOTS 16
#define RTP_HEADER_MIN_SIZE 12
#define RTP_VERSION_MASK 0xc0
#define RTP_VERSION_2 0x80
#define RTCP_PAYLOAD_TYPE_MIN 192
#define RTCP_PAYLOAD_TYPE_MAX 223
#define ETH_HEADER_SIZE 14
#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_IPV6 0x86dd
#define ETH_TYPE_VLAN 0x8100
#define ETH_TYPE_QINQ 0x88a8
#define IP_PROTOCOL_UDP 17

typedef struct {
    bool active;
    uint32_t epoch;
    uint32_t rtp_timestamp;
    int64_t capture_eof_us;
    uint8_t recorded;
} network_trace_t;

enum {
    TRACE_SRTP_FIRST = 1U << 0,
    TRACE_SRTP_MARKER = 1U << 1,
    TRACE_UDP_FIRST = 1U << 2,
    TRACE_UDP_MARKER = 1U << 3,
    TRACE_EMAC_FIRST = 1U << 4,
    TRACE_EMAC_MARKER = 1U << 5,
};

typedef struct {
    portMUX_TYPE lock;
    uint32_t epoch;
    uint32_t next_slot;
    network_trace_t traces[NETWORK_TRACE_SLOTS];
    webrtc_transport_network_timing_stats_t network;
    webrtc_transport_stage_age_summary_t stage_age;
    /* The video SSRC and the highest sequence number sent on it. */
    uint32_t video_ssrc;
    bool video_ssrc_known;
    bool video_sequence_known;
    uint16_t video_sequence;
    uint64_t rtp_retransmits;
} network_timing_ctx_t;

static network_timing_ctx_t s_network = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static uint16_t read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static bool parse_rtp(const uint8_t *rtp, size_t size, uint32_t *timestamp,
                      bool *marker)
{
    if (!rtp || size < RTP_HEADER_MIN_SIZE ||
        (rtp[0] & RTP_VERSION_MASK) != RTP_VERSION_2) {
        return false;
    }
    uint8_t marker_and_payload_type = rtp[1];
    if (marker_and_payload_type >= RTCP_PAYLOAD_TYPE_MIN &&
        marker_and_payload_type <= RTCP_PAYLOAD_TYPE_MAX) {
        return false;
    }
    *timestamp = read_be32(&rtp[4]);
    *marker = (rtp[1] & 0x80) != 0;
    return true;
}

static const uint8_t *ethernet_udp_payload(const uint8_t *frame, size_t size,
                                           size_t *payload_size)
{
    if (!frame || size < ETH_HEADER_SIZE) {
        return NULL;
    }
    size_t offset = ETH_HEADER_SIZE;
    uint16_t eth_type = read_be16(&frame[12]);
    if (eth_type == ETH_TYPE_VLAN || eth_type == ETH_TYPE_QINQ) {
        if (size < offset + 4) {
            return NULL;
        }
        eth_type = read_be16(&frame[offset + 2]);
        offset += 4;
    }
    if (eth_type == ETH_TYPE_IPV4) {
        if (size < offset + 20 || (frame[offset] >> 4) != 4) {
            return NULL;
        }
        size_t ip_header = (size_t)(frame[offset] & 0x0f) * 4U;
        if (ip_header < 20 || size < offset + ip_header + 8 ||
            frame[offset + 9] != IP_PROTOCOL_UDP) {
            return NULL;
        }
        offset += ip_header;
    } else if (eth_type == ETH_TYPE_IPV6) {
        if (size < offset + 40 + 8 || (frame[offset] >> 4) != 6 ||
            frame[offset + 6] != IP_PROTOCOL_UDP) {
            return NULL;
        }
        offset += 40;
    } else {
        return NULL;
    }
    uint16_t udp_length = read_be16(&frame[offset + 4]);
    if (udp_length < 8 || size < offset + udp_length) {
        return NULL;
    }
    *payload_size = udp_length - 8;
    return &frame[offset + 8];
}

static void add_cost(webrtc_transport_packet_cost_stats_t *cost,
                     size_t bytes, int64_t started_us, int64_t completed_us,
                     bool success)
{
    uint64_t elapsed = completed_us >= started_us ?
        (uint64_t)(completed_us - started_us) : 0;
    cost->count++;
    cost->bytes += bytes;
    cost->total_us += elapsed;
    if (elapsed > cost->max_us) {
        cost->max_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    }
    if (!success) {
        cost->errors++;
    }
}

static void add_stage_age(webrtc_transport_stage_age_stats_t *stage,
                          int64_t capture_eof_us, int64_t completed_us)
{
    if (capture_eof_us <= 0 || completed_us < capture_eof_us) {
        return;
    }
    uint64_t age = (uint64_t)(completed_us - capture_eof_us);
    stage->count++;
    stage->total_us += age;
    if (age > stage->max_us) {
        stage->max_us = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
    }
}

static void record_trace_locked(webrtc_network_stage_t stage,
                                uint32_t timestamp, bool marker,
                                int64_t completed_us)
{
    network_trace_t *trace = NULL;
    for (size_t i = 0; i < NETWORK_TRACE_SLOTS; ++i) {
        if (s_network.traces[i].active &&
            s_network.traces[i].epoch == s_network.epoch &&
            s_network.traces[i].rtp_timestamp == timestamp) {
            trace = &s_network.traces[i];
            break;
        }
    }
    if (!trace) {
        if (stage == WEBRTC_NETWORK_STAGE_SRTP) {
            s_network.network.unmatched_srtp_packets++;
        } else if (stage == WEBRTC_NETWORK_STAGE_LWIP_SENDTO) {
            s_network.network.unmatched_udp_packets++;
        } else {
            s_network.network.unmatched_emac_packets++;
        }
        return;
    }

    uint8_t first_bit = TRACE_SRTP_FIRST;
    uint8_t marker_bit = TRACE_SRTP_MARKER;
    webrtc_transport_stage_age_stats_t *first =
        &s_network.stage_age.capture_to_srtp_first;
    webrtc_transport_stage_age_stats_t *last =
        &s_network.stage_age.capture_to_srtp_marker;
    if (stage == WEBRTC_NETWORK_STAGE_LWIP_SENDTO) {
        first_bit = TRACE_UDP_FIRST;
        marker_bit = TRACE_UDP_MARKER;
        first = &s_network.stage_age.capture_to_udp_send_first;
        last = &s_network.stage_age.capture_to_udp_send_marker;
    } else if (stage == WEBRTC_NETWORK_STAGE_EMAC_HANDOFF) {
        first_bit = TRACE_EMAC_FIRST;
        marker_bit = TRACE_EMAC_MARKER;
        first = &s_network.stage_age.capture_to_emac_handoff_first;
        last = &s_network.stage_age.capture_to_emac_handoff_marker;
    }
    if (!(trace->recorded & first_bit)) {
        trace->recorded |= first_bit;
        add_stage_age(first, trace->capture_eof_us, completed_us);
    }
    if (marker && !(trace->recorded & marker_bit)) {
        trace->recorded |= marker_bit;
        add_stage_age(last, trace->capture_eof_us, completed_us);
        /* lwip_sendto() encloses esp_eth_transmit(), so its wrapper returns
         * after the EMAC handoff wrapper.  Keep the trace alive until that
         * outer marker return has been recorded. */
        if (stage == WEBRTC_NETWORK_STAGE_LWIP_SENDTO) {
            trace->active = false;
        }
    }
}

void webrtc_network_timing_reset(uint32_t epoch)
{
    webrtc_network_timing_wrappers_link_anchor();
    portENTER_CRITICAL(&s_network.lock);
    memset(s_network.traces, 0, sizeof(s_network.traces));
    memset(&s_network.network, 0, sizeof(s_network.network));
    memset(&s_network.stage_age, 0, sizeof(s_network.stage_age));
    s_network.epoch = epoch;
    s_network.stage_age.epoch = epoch;
    s_network.next_slot = 0;
    portEXIT_CRITICAL(&s_network.lock);
}

void webrtc_network_timing_begin(uint32_t epoch, uint32_t rtp_timestamp,
                                 int64_t capture_eof_us)
{
    if (!epoch || capture_eof_us <= 0) {
        return;
    }
    portENTER_CRITICAL(&s_network.lock);
    if (epoch == s_network.epoch) {
        network_trace_t *trace =
            &s_network.traces[s_network.next_slot++ % NETWORK_TRACE_SLOTS];
        if (trace->active) {
            s_network.network.trace_overwrites++;
        }
        *trace = (network_trace_t) {
            .active = true,
            .epoch = epoch,
            .rtp_timestamp = rtp_timestamp,
            .capture_eof_us = capture_eof_us,
        };
    }
    portEXIT_CRITICAL(&s_network.lock);
}

void webrtc_network_timing_cancel(uint32_t epoch, uint32_t rtp_timestamp,
                                  int64_t capture_eof_us)
{
    portENTER_CRITICAL(&s_network.lock);
    for (size_t i = 0; i < NETWORK_TRACE_SLOTS; ++i) {
        network_trace_t *trace = &s_network.traces[i];
        if (trace->active && trace->epoch == epoch &&
            trace->rtp_timestamp == rtp_timestamp &&
            trace->capture_eof_us == capture_eof_us) {
            trace->active = false;
            break;
        }
    }
    portEXIT_CRITICAL(&s_network.lock);
}

void webrtc_network_timing_set_video_ssrc(uint32_t ssrc)
{
    portENTER_CRITICAL(&s_network.lock);
    s_network.video_ssrc = ssrc;
    s_network.video_ssrc_known = true;
    s_network.video_sequence_known = false;
    portEXIT_CRITICAL(&s_network.lock);
}

/* esp_peer sends the packets of one SSRC in sequence order, and a resend is
 * the packet as it was sent before. */
static void count_retransmit_locked(const uint8_t *rtp)
{
    if (!s_network.video_ssrc_known || read_be32(&rtp[8]) != s_network.video_ssrc) {
        return;
    }
    const uint16_t sequence = read_be16(&rtp[2]);
    if (s_network.video_sequence_known &&
        (int16_t)(uint16_t)(sequence - s_network.video_sequence) <= 0) {
        s_network.rtp_retransmits++;
        return;
    }
    s_network.video_sequence = sequence;
    s_network.video_sequence_known = true;
}

void webrtc_network_timing_record_rtp(webrtc_network_stage_t stage,
                                      const uint8_t *rtp, size_t rtp_size,
                                      int64_t started_us, int64_t completed_us,
                                      bool success)
{
    uint32_t timestamp = 0;
    bool marker = false;
    if (!parse_rtp(rtp, rtp_size, &timestamp, &marker)) {
        return;
    }
    portENTER_CRITICAL(&s_network.lock);
    webrtc_transport_packet_cost_stats_t *cost =
        stage == WEBRTC_NETWORK_STAGE_SRTP ? &s_network.network.srtp_protect :
        &s_network.network.lwip_sendto;
    add_cost(cost, rtp_size, started_us, completed_us, success);
    record_trace_locked(stage, timestamp, marker, completed_us);
    if (stage == WEBRTC_NETWORK_STAGE_LWIP_SENDTO && success) {
        count_retransmit_locked(rtp);
    }
    portEXIT_CRITICAL(&s_network.lock);
}

static uint32_t fnv1a32(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; data != NULL && i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

void webrtc_network_timing_record_srtp_error(
    int status, const uint8_t *rtp, size_t rtp_size,
    size_t output_capacity, size_t output_size)
{
    if (status == srtp_err_status_ok) {
        return;
    }

    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    if (rtp != NULL && rtp_size >= RTP_HEADER_MIN_SIZE) {
        sequence = read_be16(&rtp[2]);
        timestamp = read_be32(&rtp[4]);
        ssrc = read_be32(&rtp[8]);
    }
    const uint32_t fingerprint = fnv1a32(rtp, rtp_size);

    portENTER_CRITICAL(&s_network.lock);
    switch ((srtp_err_status_t)status) {
    case srtp_err_status_replay_fail:
        s_network.network.srtp_error_replay_fail++;
        break;
    case srtp_err_status_replay_old:
        s_network.network.srtp_error_replay_old++;
        break;
    case srtp_err_status_buffer_small:
        s_network.network.srtp_error_buffer_small++;
        break;
    case srtp_err_status_auth_fail:
        s_network.network.srtp_error_auth_fail++;
        break;
    case srtp_err_status_cipher_fail:
        s_network.network.srtp_error_cipher_fail++;
        break;
    default:
        s_network.network.srtp_error_other++;
        break;
    }
    s_network.network.srtp_last_error_status = status;
    s_network.network.srtp_last_error_sequence = sequence;
    s_network.network.srtp_last_error_timestamp = timestamp;
    s_network.network.srtp_last_error_ssrc = ssrc;
    s_network.network.srtp_last_error_input_bytes =
        rtp_size > UINT32_MAX ? UINT32_MAX : (uint32_t)rtp_size;
    s_network.network.srtp_last_error_output_capacity =
        output_capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)output_capacity;
    s_network.network.srtp_last_error_output_bytes =
        output_size > UINT32_MAX ? UINT32_MAX : (uint32_t)output_size;
    s_network.network.srtp_last_error_fingerprint = fingerprint;
    portEXIT_CRITICAL(&s_network.lock);
}

void webrtc_network_timing_record_srtp_breakdown(
    const uint8_t *rtp, size_t rtp_size, uint32_t cipher_us,
    uint32_t auth_and_framework_us)
{
    uint32_t timestamp = 0;
    bool marker = false;
    if (!parse_rtp(rtp, rtp_size, &timestamp, &marker)) {
        return;
    }
    (void)timestamp;
    (void)marker;
    portENTER_CRITICAL(&s_network.lock);
    add_cost(&s_network.network.srtp_cipher, rtp_size, 0, cipher_us, true);
    add_cost(&s_network.network.srtp_auth_and_framework, rtp_size, 0,
             auth_and_framework_us, true);
    portEXIT_CRITICAL(&s_network.lock);
}

void webrtc_network_timing_record_ethernet(
    const uint8_t *frame, size_t frame_size, int64_t started_us,
    int64_t completed_us, bool success)
{
    size_t rtp_size = 0;
    const uint8_t *rtp = ethernet_udp_payload(frame, frame_size, &rtp_size);
    uint32_t timestamp = 0;
    bool marker = false;
    if (!parse_rtp(rtp, rtp_size, &timestamp, &marker)) {
        return;
    }
    portENTER_CRITICAL(&s_network.lock);
    add_cost(&s_network.network.emac_handoff, frame_size, started_us,
             completed_us, success);
    record_trace_locked(WEBRTC_NETWORK_STAGE_EMAC_HANDOFF, timestamp, marker,
                        completed_us);
    portEXIT_CRITICAL(&s_network.lock);
}

void webrtc_network_timing_snapshot(
    webrtc_transport_network_timing_stats_t *network,
    webrtc_transport_stage_age_summary_t *stage_age)
{
    portENTER_CRITICAL(&s_network.lock);
    *network = s_network.network;
    network->rtp_retransmits = s_network.rtp_retransmits;
    stage_age->capture_to_srtp_first =
        s_network.stage_age.capture_to_srtp_first;
    stage_age->capture_to_srtp_marker =
        s_network.stage_age.capture_to_srtp_marker;
    stage_age->capture_to_udp_send_first =
        s_network.stage_age.capture_to_udp_send_first;
    stage_age->capture_to_udp_send_marker =
        s_network.stage_age.capture_to_udp_send_marker;
    stage_age->capture_to_emac_handoff_first =
        s_network.stage_age.capture_to_emac_handoff_first;
    stage_age->capture_to_emac_handoff_marker =
        s_network.stage_age.capture_to_emac_handoff_marker;
    portEXIT_CRITICAL(&s_network.lock);
}
