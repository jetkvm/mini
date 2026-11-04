#!/usr/bin/env python3
"""An upload's hold on video also stops the host's sound.

Runs the transport's send_pending_audio() against the audio component's own
queue functions, and the stats renderer of "videoPausedBy" and
"audioPausedBy" with real cJSON."""

from pathlib import Path
import re
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
TRANSPORT = (COMPONENT / "webrtc_transport.c").read_text()
RENDERER = (COMPONENT / "webrtc_stats_json.c").read_text()
HEADER = (COMPONENT / "include" / "webrtc_transport.h").read_text()
AUDIO = (REPO / "components" / "jetkvm_audio" / "jetkvm_audio.c").read_text()
AUDIO_HEADER = (REPO / "components" / "jetkvm_audio" / "include" / "jetkvm_audio.h").read_text()


def function(source, signature):
    """The definition that starts with signature, to its closing brace."""
    start = source.index(signature)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos] + "\n"


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


# Both views of /webrtc/stats report the two lists.
full = between(RENDERER, "static esp_err_t stats_json_create(",
               "\nesp_err_t webrtc_stats_json_create(")
compact = RENDERER[RENDERER.index("esp_err_t webrtc_stats_json_create_bitrate("):]
assert full.count("add_paused_streams(root, stats.video_pause_sources);") == 1
assert compact.count("add_paused_streams(root, stats.video_pause_sources);") == 1

# The peer task sends sound before and after each video frame, so it also
# sees a hold that begins or ends within one turn of its loop.
assert TRANSPORT.count("send_pending_audio(ctx);") == 2

sources = re.search(r"typedef enum \{[^}]*\} webrtc_video_pause_source_t;", HEADER)[0]
audio_sources = re.search(r"#define WEBRTC_AUDIO_PAUSE_SOURCES .*", HEADER)[0]
slots = re.search(r"#define JETKVM_AUDIO_PACKET_SLOTS \d+", AUDIO_HEADER)[0]

PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define JETKVM_AUDIO_PACKET_MAX 1275
""" + slots + r"""
#define PACKET_SLOTS JETKVM_AUDIO_PACKET_SLOTS
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)

/* The audio component's state, as jetkvm_audio.c has it. */
typedef struct { uint32_t pts; int size; uint8_t data[JETKVM_AUDIO_PACKET_MAX]; } packet_t;
static void *s_mutex = (void *)1;
static packet_t packets[PACKET_SLOTS], *s_packets = packets;
static struct { unsigned dropped_frames; } s_stats;
static unsigned s_read, s_count;
static uint32_t s_generation;
static bool s_enabled = true, s_usb_ready = true, s_streaming = true;
static bool s_peer_connected, s_usb_paused, held;
static int xSemaphoreTake(void *mutex, unsigned ticks)
{
    (void)ticks;
    assert(mutex && !held);
    held = true;
    return pdTRUE;
}
static void xSemaphoreGive(void *mutex) { assert(mutex && held); held = false; }

/* The transport's state that send_pending_audio() reads, and its peer. */
typedef struct { uint8_t *data; int size; uint32_t pts; } esp_peer_audio_frame_t;
typedef struct {
    int lock;
    bool peer_connected;
    uint8_t video_pause_sources;
    void *peer;
} transport_ctx_t;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static unsigned sent;
static uint32_t last_sent_pts;
static int esp_peer_send_audio(void *peer, esp_peer_audio_frame_t *frame)
{
    assert(peer && frame->size == 3);
    ++sent;
    last_sent_pts = frame->pts;
    return 0;
}
"""

CHECKS = r"""
static const uint8_t opus[] = {1, 2, 3};
/* One turn of the peer task's loop: sound, the video frame, sound. */
static unsigned turn(transport_ctx_t *ctx)
{
    unsigned before = sent;
    send_pending_audio(ctx);
    send_pending_audio(ctx);
    return sent - before;
}
/* The audio worker has encoded one frame of the capture segment that began
 * at `generation`. */
static void encoded(uint32_t generation, uint32_t pts)
{
    queue_packet(generation, opus, sizeof(opus), pts);
}
static char *render(uint8_t held_sources)
{
    cJSON *root = cJSON_CreateObject();
    assert(root);
    add_paused_streams(root, held_sources);
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    cJSON_Delete(root);
    return json;
}
static void expect(uint8_t held_sources, const char *json)
{
    char *rendered = render(held_sources);
    if (strcmp(rendered, json) != 0) {
        fprintf(stderr, "sources %u: %s, expected %s\n", held_sources, rendered, json);
        exit(1);
    }
    free(rendered);
}
int main(void)
{
    transport_ctx_t ctx = { .peer = &ctx, .peer_connected = true };

    /* Without a hold the sound goes to the peer. */
    assert(turn(&ctx) == 0 && s_peer_connected);
    uint32_t segment = s_generation;
    encoded(segment, 1000);
    encoded(segment, 1020);
    assert(turn(&ctx) == 2 && last_sent_pts == 1020);

    /* An upload takes the writer lease while three frames wait. None of
     * them is sent, and a frame that the worker finishes after the hold
     * began is not taken either. */
    encoded(segment, 1040);
    encoded(segment, 1060);
    encoded(segment, 1080);
    ctx.video_pause_sources = WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD;
    assert(turn(&ctx) == 0 && !s_peer_connected && s_count == 0);
    assert(s_generation != segment);
    encoded(segment, 1100);
    encoded(s_generation, 1120);
    assert(turn(&ctx) == 0 && s_count == 0);
    /* The worker reads this to stop encoding and to keep the USB FIFO empty. */
    assert(!s_peer_connected);

    /* The upload ends after 12 s. The worker starts a new capture segment,
     * and its first frame is the first one sent: no frame of the time
     * before the hold or during it follows. */
    uint32_t during = s_generation;
    ctx.video_pause_sources = 0;
    assert(turn(&ctx) == 0 && s_peer_connected && s_generation != during);
    encoded(during, 13100);
    assert(turn(&ctx) == 0);
    encoded(s_generation, 13120);
    assert(turn(&ctx) == 1 && last_sent_pts == 13120);

    /* The holds that leave the link to the sound do not stop it: the page's
     * own pause, a firmware update's and the Mini's download. A second hold
     * beside the upload's does not end the stop when it goes. */
    const uint8_t others = WEBRTC_VIDEO_PAUSE_UI | WEBRTC_VIDEO_PAUSE_OTA |
                           WEBRTC_VIDEO_PAUSE_STORAGE_DOWNLOAD;
    ctx.video_pause_sources = others;
    encoded(s_generation, 13140);
    assert(turn(&ctx) == 1 && s_peer_connected);
    ctx.video_pause_sources = others | WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD;
    encoded(s_generation, 13160);
    assert(turn(&ctx) == 0 && !s_peer_connected);
    ctx.video_pause_sources = WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD;
    assert(turn(&ctx) == 0 && !s_peer_connected);
    ctx.video_pause_sources = WEBRTC_VIDEO_PAUSE_UI;
    assert(turn(&ctx) == 0 && s_peer_connected);
    encoded(s_generation, 13180);
    assert(turn(&ctx) == 1);

    /* A hold that ends while no peer is connected starts no sound. */
    ctx.peer_connected = false;
    ctx.video_pause_sources = 0;
    assert(turn(&ctx) == 0 && !s_peer_connected);

    /* The stats name the upload for both streams, and the other sources
     * for video only. */
    expect(0, "{\"videoPausedBy\":[],\"audioPausedBy\":[]}");
    expect(WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD,
           "{\"videoPausedBy\":[\"upload\"],\"audioPausedBy\":[\"upload\"]}");
    expect(others, "{\"videoPausedBy\":[\"ui\",\"ota\",\"download\"],\"audioPausedBy\":[]}");
    expect(WEBRTC_VIDEO_PAUSE_OTA | WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD,
           "{\"videoPausedBy\":[\"ota\",\"upload\"],\"audioPausedBy\":[\"upload\"]}");
    puts("sound stops with video for an upload and starts again with a new segment: PASS");
    return 0;
}
"""

production = (
    sources + "\n" + audio_sources + "\n" +
    "".join(function(AUDIO, signature) for signature in [
        "static void invalidate(", "static bool lock(", "static void unlock(",
        "static void queue_packet(", "void jetkvm_audio_set_peer_connected(",
        "bool jetkvm_audio_pop(",
    ]) +
    function(TRANSPORT, "static void send_pending_audio(") +
    function(RENDERER, "static void add_paused_by(") +
    function(RENDERER, "static void add_paused_streams(")
)

with tempfile.TemporaryDirectory(prefix="webrtc-audio-pause-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(PREAMBLE + production + CHECKS)
    cjson = REPO / "managed_components/espressif__cjson/cJSON"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-fsanitize=address,undefined",
                    "-I", str(cjson), str(temp / "test.c"), str(cjson / "cJSON.c"),
                    "-o", str(temp / "test"), "-lm"], check=True)
    subprocess.run([str(temp / "test")], check=True)
