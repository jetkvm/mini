#!/usr/bin/env python3
"""The diagnostics of a service video below 60 fps in /webrtc/stats?bitrate=1:
the peer-task passes and their bucket above one frame at 60 fps, the idle
runs per core, and the video RTP packets that esp_peer sent again
(network_timing.c, built with stub FreeRTOS and libsrtp headers)."""

from pathlib import Path
import re
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
TRANSPORT = (COMPONENT / "webrtc_transport.c").read_text()
RENDERER = (COMPONENT / "webrtc_stats_json.c").read_text()
HEADER = (COMPONENT / "include" / "webrtc_transport.h").read_text()


def function(text, signature):
    """The definition that starts with signature, to its closing brace."""
    start = text.index(signature)
    pos = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos] + "\n"


# Next to peerLoop in the compact view, which the combined_load recorder
# keeps whole; rtpRetransmits also in the full view's networkTiming.
compact = RENDERER[RENDERER.index("esp_err_t webrtc_stats_json_create_bitrate("):]
assert compact.count("add_peer_pass(root, &stats.peer_pass);") == 1
assert compact.count("add_idle_runs(root, stats.idle_runs, stats.idle_counted);") == 1
assert '"rtpRetransmits", stats.network_timing.rtp_retransmits' in compact
assert 'add_u64(network, "rtpRetransmits", stats->rtp_retransmits);' in \
    function(RENDERER, "static void add_network_timing(")
# The whole pass from the wake, with the delay for a starved IDLE task.
assert re.search(r"ulTaskNotifyTake\(pdTRUE, pdMS_TO_TICKS\(10\)\);\s*(/\*.*?\*/\s*)?"
                 r"const int64_t woke_us = esp_timer_get_time\(\);", TRANSPORT, re.S)
assert re.search(r"record_peer_pass\(&ctx->stats\.peer_pass,\s*"
                 r"phase_us\[WEBRTC_PEER_PHASE_COUNT\] - woke_us\);", TRANSPORT)
# Only the packets of esp_peer_send_video() name the video SSRC.
assert re.search(r"ctx->sending_video = true;\s*int ret = esp_peer_send_video\(ctx->peer, &frame\);"
                 r"\s*ctx->sending_video = false;", TRANSPORT)

pass_type = re.search(r"typedef struct \{[^{}]*\} webrtc_transport_peer_pass_t;", HEADER)[0]

STUB_FREERTOS = r"""
#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
"""
STUB_SRTP = r"""
#pragma once
typedef enum {
    srtp_err_status_ok = 0, srtp_err_status_fail, srtp_err_status_auth_fail,
    srtp_err_status_cipher_fail, srtp_err_status_replay_fail,
    srtp_err_status_replay_old, srtp_err_status_buffer_small,
} srtp_err_status_t;
"""
STUB_ESP_ERR = "#pragma once\ntypedef int esp_err_t;\n"

PASSES_AND_IDLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define WEBRTC_TRANSPORT_IDLE_CORES 2
""" + pass_type + "\n" + function(TRANSPORT, "static void record_peer_pass(") + \
    function(RENDERER, "static void add_u64(") + \
    function(RENDERER, "static void add_peer_pass(") + \
    function(RENDERER, "static void add_idle_runs(") + r"""
static void expect_json(cJSON *root, const char *expected)
{
    char *json = cJSON_PrintUnformatted(root);
    if (strcmp(json, expected) != 0) {
        fprintf(stderr, "got %s\nwant %s\n", json, expected);
        abort();
    }
    free(json);
    cJSON_Delete(root);
}

int main(void)
{
    webrtc_transport_peer_pass_t pass = {0};
    /* One frame at 60 fps is 16666.7 us: a pass of 16666 us keeps up. */
    record_peer_pass(&pass, 16666);
    assert(pass.count == 1 && pass.over_16ms == 0 && pass.max_us == 16666);
    record_peer_pass(&pass, 16667);
    record_peer_pass(&pass, 1200);
    record_peer_pass(&pass, 50000);
    record_peer_pass(&pass, -1);
    assert(pass.count == 4 && pass.over_16ms == 2);
    assert(pass.sum_us == 16666 + 16667 + 1200 + 50000 && pass.max_us == 50000);
    /* Sums past 32 bits keep every microsecond. */
    pass.sum_us = 4294967296ULL + 7;
    cJSON *root = cJSON_CreateObject();
    add_peer_pass(root, &pass);
    expect_json(root, "{\"peerPass\":{\"count\":4,\"sumUs\":4294967303,"
                      "\"over16ms\":2,\"maxUs\":50000}}");

    const uint64_t runs[WEBRTC_TRANSPORT_IDLE_CORES] = {5000000000ULL, 12};
    const bool both[WEBRTC_TRANSPORT_IDLE_CORES] = {true, true};
    root = cJSON_CreateObject();
    add_idle_runs(root, runs, both);
    expect_json(root, "{\"idleRuns\":[5000000000,12]}");
    const bool first[WEBRTC_TRANSPORT_IDLE_CORES] = {true, false};
    root = cJSON_CreateObject();
    add_idle_runs(root, runs, first);
    expect_json(root, "{\"idleRuns\":[5000000000,null]}");
    puts("peerPass and idleRuns in /webrtc/stats: PASS");
    return 0;
}
"""

RETRANSMITS = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "network_timing.h"

void webrtc_network_timing_wrappers_link_anchor(void) {}

static uint8_t packet[64];

/* An RTP (or SRTP: its header is in the clear) packet. */
static const uint8_t *rtp(uint32_t ssrc, uint16_t sequence)
{
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x80;
    packet[1] = 102;
    packet[2] = (uint8_t)(sequence >> 8);
    packet[3] = (uint8_t)sequence;
    packet[8] = (uint8_t)(ssrc >> 24);
    packet[9] = (uint8_t)(ssrc >> 16);
    packet[10] = (uint8_t)(ssrc >> 8);
    packet[11] = (uint8_t)ssrc;
    return packet;
}

static void sent(uint32_t ssrc, uint16_t sequence, bool success)
{
    webrtc_network_timing_record_rtp(WEBRTC_NETWORK_STAGE_LWIP_SENDTO,
                                     rtp(ssrc, sequence), sizeof(packet), 0, 1,
                                     success);
}

static uint64_t retransmits(void)
{
    webrtc_transport_network_timing_stats_t network;
    webrtc_transport_stage_age_summary_t stage_age;
    webrtc_network_timing_snapshot(&network, &stage_age);
    return network.rtp_retransmits;
}

enum { VIDEO = 0x11223344, AUDIO = 0x55667788 };

int main(void)
{
    /* Before esp_peer_send_video() named the video SSRC nothing counts. */
    sent(VIDEO, 100, true);
    sent(VIDEO, 100, true);
    assert(retransmits() == 0);

    webrtc_network_timing_set_video_ssrc(VIDEO);
    sent(VIDEO, 100, true);
    sent(VIDEO, 101, true);
    sent(VIDEO, 102, true);
    assert(retransmits() == 0);
    /* A NACK: esp_peer sends packets 101 and 102 again, as they were. */
    sent(VIDEO, 101, true);
    sent(VIDEO, 102, true);
    assert(retransmits() == 2);
    sent(VIDEO, 103, true);
    assert(retransmits() == 2);

    /* Audio, an older sequence on another SSRC, does not count. */
    sent(AUDIO, 7, true);
    sent(AUDIO, 6, true);
    assert(retransmits() == 2);

    /* Neither does the SRTP stage, which sees each new packet once. */
    webrtc_network_timing_record_rtp(WEBRTC_NETWORK_STAGE_SRTP, rtp(VIDEO, 50),
                                     sizeof(packet), 0, 1, true);
    assert(retransmits() == 2);

    /* RTCP, and a packet in a TURN relay's ChannelData framing. */
    rtp(VIDEO, 1);
    packet[1] = 200;
    webrtc_network_timing_record_rtp(WEBRTC_NETWORK_STAGE_LWIP_SENDTO, packet,
                                     sizeof(packet), 0, 1, true);
    rtp(VIDEO, 1);
    packet[0] = 0x40;
    webrtc_network_timing_record_rtp(WEBRTC_NETWORK_STAGE_LWIP_SENDTO, packet,
                                     sizeof(packet), 0, 1, true);
    assert(retransmits() == 2);

    /* A send that failed did not go out; its resend is the first. */
    sent(VIDEO, 104, false);
    sent(VIDEO, 104, true);
    assert(retransmits() == 2);

    /* Across the wrap of the 16-bit sequence. */
    for (uint32_t sequence = 105; sequence <= 65535 + 2; ++sequence) {
        sent(VIDEO, (uint16_t)sequence, true);
    }
    assert(retransmits() == 2);
    sent(VIDEO, 65535, true);
    sent(VIDEO, 1, true);
    assert(retransmits() == 4);

    /* A new peer, also with the same SSRC, starts its own sequence; the
     * count of the video epoch's other fields restarts, this one stays. */
    webrtc_network_timing_set_video_ssrc(VIDEO);
    sent(VIDEO, 10, true);
    sent(VIDEO, 11, true);
    webrtc_network_timing_reset(2);
    assert(retransmits() == 4);
    sent(VIDEO, 11, true);
    assert(retransmits() == 5);
    puts("networkTiming.rtpRetransmits: PASS");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="webrtc-stats-video-diagnostics-") as directory:
    temp = Path(directory)
    cjson = REPO / "managed_components/espressif__cjson/cJSON"
    (temp / "passes.c").write_text(PASSES_AND_IDLE)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-fsanitize=address,undefined", "-I", str(cjson),
                    str(temp / "passes.c"), str(cjson / "cJSON.c"),
                    "-o", str(temp / "passes"), "-lm"], check=True)
    subprocess.run([str(temp / "passes")], check=True)

    (temp / "freertos").mkdir()
    (temp / "freertos" / "FreeRTOS.h").write_text(STUB_FREERTOS)
    (temp / "srtp.h").write_text(STUB_SRTP)
    (temp / "esp_err.h").write_text(STUB_ESP_ERR)
    (temp / "retransmits.c").write_text(RETRANSMITS)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-fsanitize=address,undefined",
                    "-I", str(temp), "-I", str(COMPONENT), "-I", str(COMPONENT / "include"),
                    str(temp / "retransmits.c"), str(COMPONENT / "network_timing.c"),
                    "-o", str(temp / "retransmits")], check=True)
    subprocess.run([str(temp / "retransmits")], check=True)
