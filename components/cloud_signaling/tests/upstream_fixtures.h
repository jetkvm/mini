#pragma once

/*
 * Wire fixtures transcribed from JetKVM upstream b3c29a44:
 *   web.go:41-47, 453-485       device-side offer/candidate decoding
 *   cloud.go:494               answer envelope
 *   webrtc.go:587-595          device-side candidate envelope
 *   ui/src/routes/devices.$id.tsx:496-528
 *                              browser SDP base64 and ICE emission
 * SDP/candidate values are deterministic test values; the envelope shape and
 * capitalization are the upstream protocol (not an ESP32-specific variant).
 */

#define UPSTREAM_OFFER_SDP                                                  \
    "v=0\r\n"                                                             \
    "o=- 46117317 2 IN IP4 127.0.0.1\r\n"                                \
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"                                 \
    "a=rtpmap:96 H264/90000\r\n"

#define UPSTREAM_OFFER_BASE64                                               \
    "eyJ0eXBlIjoib2ZmZXIiLCJzZHAiOiJ2PTBcclxubz0tIDQ2MTE3MzE3IDIgSU4g"   \
    "SVA0IDEyNy4wLjAuMVxyXG5tPXZpZGVvIDkgVURQL1RMUy9SVFAvU0FWUEYgOTZc"   \
    "clxuYT1ydHBtYXA6OTYgSDI2NC85MDAwMFxyXG4ifQ=="

#define UPSTREAM_OFFER_ENVELOPE                                             \
    "{\"type\":\"offer\",\"data\":{"                                      \
    "\"sd\":\"" UPSTREAM_OFFER_BASE64 "\","                                \
    "\"ip\":\"203.0.113.7\","                                             \
    "\"iceServers\":[\"stun:stun.jetkvm.com:3478\","                       \
    "\"turn:turn.jetkvm.com:3478?transport=udp\"],"                         \
    "\"OidcGoogle\":\"header.payload.signature\"}}"

#define UPSTREAM_ANSWER_SDP                                                 \
    "v=0\r\n"                                                             \
    "o=- 999 2 IN IP4 192.168.1.2\r\n"                                    \
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"

#define UPSTREAM_ANSWER_BASE64                                              \
    "eyJ0eXBlIjoiYW5zd2VyIiwic2RwIjoidj0wXHJcbm89LSA5OTkgMiBJTiBJUDQg"   \
    "MTkyLjE2OC4xLjJcclxubT12aWRlbyA5IFVEUC9UTFMvUlRQL1NBVlBGIDk2XHJc"   \
    "biJ9"

#define UPSTREAM_ANSWER_ENVELOPE                                            \
    "{\"type\":\"answer\",\"data\":\"" UPSTREAM_ANSWER_BASE64 "\"}"

#define UPSTREAM_CANDIDATE_VALUE                                            \
    "candidate:842163049 1 udp 1677734911 203.0.113.7 50000 typ srflx "     \
    "raddr 192.168.1.2 rport 50000 generation 0 ufrag p4Kvm network-cost 999"

#define UPSTREAM_CANDIDATE_ENVELOPE                                         \
    "{\"type\":\"new-ice-candidate\",\"data\":{"                          \
    "\"candidate\":\"" UPSTREAM_CANDIDATE_VALUE "\","                      \
    "\"sdpMid\":\"0\",\"sdpMLineIndex\":0,"                              \
    "\"usernameFragment\":\"p4Kvm\"}}"

/* Pion ICECandidate.ToJSON leaves UsernameFragment nil; encoding/json emits
 * the non-omitempty field as null (webrtc/v4.2.1 icecandidate*.go). */
#define UPSTREAM_DEVICE_CANDIDATE_WITH_NULL_USERNAME                        \
    "{\"type\":\"new-ice-candidate\",\"data\":{"                          \
    "\"candidate\":\"candidate:1 1 udp 1 192.0.2.1 9 typ host\","           \
    "\"sdpMid\":\"0\",\"sdpMLineIndex\":0,"                              \
    "\"usernameFragment\":null}}"
