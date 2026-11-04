#!/usr/bin/env python3
"""Drive the production audio worker against real TinyUSB FIFO overflow behavior.

The worker mixes each stereo frame down to mono before the encoder, so the
encoder provider sees half the bytes of a frame; a stereo frame filled with
one byte value gives a mono frame filled with the same value.

Only scheduling, USB arrival timing, the encoder and packet sink are providers.
The worker and timestamp helpers are extracted unchanged; tusb_fifo.c is compiled
from the managed dependency so its overwrite correction is not a test model.
"""
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
SOURCE = (ROOT / "components/jetkvm_audio/jetkvm_audio.c").read_text()
TINYUSB = ROOT / "managed_components/espressif__tinyusb/src"


def function(name):
    match = re.search(
        r"^(?:static )?[^\n]+\b" + name + r"\([^;]*?\)\s*\{", SOURCE, re.M
    )
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end] + "\n"


PREFIX = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "osal/osal.h"
#include "common/tusb_fifo.h"

#define PCM_BYTES (960 * 2 * sizeof(int16_t))
#define MONO_BYTES (960 * sizeof(int16_t))
#define MAX_EVENTS 6000
#define ESP_AUDIO_ERR_OK 0
typedef int esp_audio_err_t;
typedef struct { uint8_t *buffer; size_t len; } esp_audio_enc_in_frame_t;
typedef struct { uint8_t *buffer; size_t len, encoded_bytes; } esp_audio_enc_out_frame_t;
static uint8_t pcm[PCM_BYTES], encoded[8], fifo_storage[8192];
static uint8_t *s_pcm = pcm, *s_encoded = encoded;
static int s_encoded_capacity = sizeof(encoded);
static void *s_encoder = (void *)1;
static uint32_t s_generation;
static bool s_usb_ready, s_usb_paused, s_enabled, s_peer_connected, s_streaming;
static struct { unsigned pcm_bytes, encoded_frames, encode_errors; } s_stats;
static tu_fifo_t fifo;
static bool held;

typedef struct {
    uint64_t now_ms;
    unsigned bytes;
    uint8_t value;
    unsigned racing_bytes;
    uint8_t racing_value;
    bool new_generation;
} event_t;
typedef struct {
    uint32_t pts, generation;
    unsigned event;
    uint8_t pcm[MONO_BYTES];
} packet_t;
static event_t events[MAX_EVENTS];
static packet_t packets[MAX_EVENTS];
static unsigned event_count, event_index, packet_count, codec_resets;
static unsigned read_requested[MAX_EVENTS], read_returned[MAX_EVENTS];
static bool overflow_before_read[MAX_EVENTS];
static uint64_t now_ms;
static jmp_buf task_finished;

/* USB OUT arrives in 1 ms stereo 48 kHz chunks. In particular, don't use a
 * single oversized FIFO write: TinyUSB handles that differently from repeated
 * writes that overtake a stalled reader twice. */
static void arrive(unsigned bytes, uint8_t value)
{
    uint8_t data[192];
    memset(data, value, sizeof(data));
    while (bytes) {
        unsigned n = bytes < sizeof(data) ? bytes : sizeof(data);
        assert(tu_fifo_write_n(&fifo, data, n) == n);
        bytes -= n;
    }
}

static void begin_event(void)
{
    event_t *event = &events[event_index];
    now_ms = event->now_ms;
    if (event->new_generation) ++s_generation;
    arrive(event->bytes, event->value);
}

static void schedule_next(void)
{
    assert(!held);
    if (++event_index == event_count) longjmp(task_finished, 1);
    begin_event();
}

static void vTaskDelay(unsigned ticks) { assert(ticks == 1); schedule_next(); }
static void taskYIELD(void) { schedule_next(); }
static bool lock(void) { assert(!held); held = true; return true; }
static void unlock(void) { assert(held); held = false; }
static int64_t esp_timer_get_time(void) { return (int64_t)now_ms * 1000; }
static tu_fifo_t *tud_audio_get_ep_out_ff(void) { assert(held); return &fifo; }
static uint16_t tud_audio_available(void) { assert(held); return tu_fifo_count(&fifo); }
static uint16_t tud_audio_read(void *buffer, uint16_t count)
{
    assert(held);
    event_t *event = &events[event_index];
    read_requested[event_index] = count;
    overflow_before_read[event_index] = tu_fifo_overflowed(&fifo);
    /* This arrival is deliberately after the worker's overflow precheck and
     * saved rd_idx, but before TinyUSB snapshots the indices for read_n. */
    arrive(event->racing_bytes, event->racing_value);
    uint16_t result = tu_fifo_read_n(&fifo, buffer, count);
    read_returned[event_index] = result;
    return result;
}

static void esp_opus_enc_reset(void *encoder)
{
    assert(encoder == s_encoder && held);
    ++codec_resets;
}

static esp_audio_err_t esp_opus_enc_process(void *encoder,
                                           esp_audio_enc_in_frame_t *input,
                                           esp_audio_enc_out_frame_t *output)
{
    assert(!held && encoder == s_encoder && input->len == MONO_BYTES);
    assert(output->len >= 1 && packet_count < MAX_EVENTS);
    memcpy(packets[packet_count].pcm, input->buffer, MONO_BYTES);
    output->buffer[0] = 0xab;
    output->encoded_bytes = 1;
    return ESP_AUDIO_ERR_OK;
}

static void queue_packet(uint32_t generation, const uint8_t *data,
                         size_t size, uint32_t pts)
{
    assert(!held && size == 1 && data[0] == 0xab);
    assert(generation == s_generation);
    packet_t *packet = &packets[packet_count++];
    packet->generation = generation;
    packet->pts = pts;
    packet->event = event_index;
}
'''

MAIN = r'''
static void setup(void)
{
    memset(events, 0, sizeof(events));
    memset(packets, 0, sizeof(packets));
    memset(read_requested, 0, sizeof(read_requested));
    memset(read_returned, 0, sizeof(read_returned));
    memset(overflow_before_read, 0, sizeof(overflow_before_read));
    memset(&s_stats, 0, sizeof(s_stats));
    assert(tu_fifo_config(&fifo, fifo_storage, sizeof(fifo_storage), 1, true));
    event_count = event_index = packet_count = codec_resets = 0;
    s_generation = 10;
    held = s_usb_paused = false;
    s_usb_ready = s_enabled = s_peer_connected = s_streaming = true;
}

static void add(uint64_t time, unsigned bytes, uint8_t value)
{
    assert(event_count < MAX_EVENTS);
    events[event_count++] = (event_t){.now_ms=time, .bytes=bytes, .value=value};
}

static void run(void)
{
    assert(event_count);
    begin_event();
    if (setjmp(task_finished) == 0) audio_task(NULL);
    assert(!held && s_stats.encode_errors == 0);
    assert(s_stats.encoded_frames == packet_count);
}

/* begin and end are bytes of the stereo frame from USB; two of them are one
 * byte of the mono frame that reached the encoder. */
static void samples(unsigned packet, unsigned begin, unsigned end, uint8_t value)
{
    assert(packet < packet_count && begin <= end && end <= PCM_BYTES);
    assert(begin % 4 == 0 && end % 4 == 0);
    for (unsigned i=begin/2; i<end/2; ++i) assert(packets[packet].pcm[i] == value);
}

static void put(uint8_t *frame, unsigned at, int left, int right)
{
    const int16_t pair[2] = {(int16_t)left, (int16_t)right};
    memcpy(frame + at * sizeof(pair), pair, sizeof(pair));
}

static int mono(const uint8_t *frame, unsigned at)
{
    int16_t value;
    memcpy(&value, frame + at * sizeof(value), sizeof(value));
    return value;
}

/* What the mix of left and right does to the level: the same sound on both
 * channels keeps it, sound on one channel alone has half the amplitude,
 * and opposite sound on the two channels is silence. No pair overflows. */
static void downmix_levels(void)
{
    uint8_t frame[8 * 2 * sizeof(int16_t)];
    put(frame, 0, 1000, 1000);
    put(frame, 1, 1000, 0);
    put(frame, 2, 0, -1000);
    put(frame, 3, 1000, -1000);
    put(frame, 4, 32767, 32767);
    put(frame, 5, -32768, -32768);
    put(frame, 6, 32767, -32768);
    put(frame, 7, -3, -4);
    downmix_to_mono(frame, sizeof(frame));
    assert(mono(frame, 0) == 1000 && mono(frame, 1) == 500 && mono(frame, 2) == -500);
    assert(mono(frame, 3) == 0 && mono(frame, 4) == 32767 && mono(frame, 5) == -32768);
    assert(mono(frame, 6) == 0 && mono(frame, 7) == -3);
}

static void clock_drift(int direction)
{
    setup();
    for (unsigned i=0; i<MAX_EVENTS; ++i) {
        /* Two minutes, +/-1000 ppm: 120 ms of accumulated drift, while every
         * individual receive interval stays healthy. */
        int64_t time = 1000 + i*20 + direction*(int64_t)(i/50);
        add((uint64_t)time, PCM_BYTES, (uint8_t)i);
    }
    run();
    assert(packet_count == MAX_EVENTS && codec_resets == 1);
    for (unsigned i=0; i<packet_count; ++i) {
        assert(packets[i].pts == 980 + i*20);
        samples(i, 0, PCM_BYTES, (uint8_t)i);
    }
}

static void overflow_during_read(void)
{
    setup();
    add(1000, PCM_BYTES, 0x11);
    add(1010, PCM_BYTES/2, 0x22); /* Old partial must not reach the encoder. */
    add(1030, 8000, 0x33);
    events[2].racing_bytes = 384;
    events[2].racing_value = 0x44;
    add(1040, 0, 0);
    run();
    assert(!overflow_before_read[2]);
    assert(read_requested[2] == PCM_BYTES/2 && read_returned[2] == PCM_BYTES/2);
    assert(packet_count == 2 && packets[1].event == 3);
    assert(packets[1].pts == 1008); /* New full frame, 2432 B still in FIFO. */
    samples(1, 0, PCM_BYTES, 0x33);
}

static void overflow_already_visible(void)
{
    setup();
    add(1000, PCM_BYTES, 0x11);
    add(1010, PCM_BYTES/2, 0x22);
    add(1030, 8384, 0x33);
    add(1040, 0, 0);
    run();
    assert(overflow_before_read[2] && read_requested[2] == PCM_BYTES);
    assert(packet_count == 2 && packets[1].event == 3);
    assert(packets[1].pts == 1018); /* The corrected read itself was discarded. */
    samples(1, 0, PCM_BYTES, 0x33);
}

static void pause_discards_partial(bool first_frame)
{
    setup();
    if (!first_frame) add(990, PCM_BYTES, 0x11);
    add(1000, PCM_BYTES/2, 0x22);
    /* Empty polling must not advance the last successful receive time. */
    add(1010, 0, 0);
    add(1020, 0, 0);
    add(1030, 0, 0);
    add(1041, PCM_BYTES, 0x33);
    run();
    unsigned recovered = first_frame ? 0 : 1;
    assert(packet_count == recovered + 1);
    assert(read_requested[event_count-1] == PCM_BYTES);
    assert(packets[recovered].pts == 1021);
    samples(recovered, 0, PCM_BYTES, 0x33);
}

static void forty_ms_continuity(void)
{
    setup();
    add(990, PCM_BYTES, 0x11);
    add(1000, PCM_BYTES/2, 0x22);
    add(1040, PCM_BYTES/2, 0x33);
    run();
    assert(packet_count == 2 && packets[1].pts == 990);
    samples(1, 0, PCM_BYTES/2, 0x22);
    samples(1, PCM_BYTES/2, PCM_BYTES, 0x33);
}

static void double_overflow_hidden_by_fifo(void)
{
    setup();
    add(1000, PCM_BYTES, 0x11);
    add(1010, 3500, 0x22);
    add(1020, 6340, 0x33); /* Complete 340 B, leave 6000 B backlogged. */
    add(1075, 55*192, 0x44); /* 55 ms: writer normalizes double overflow. */
    run();
    assert(packet_count == 3 && !overflow_before_read[3]);
    assert(read_returned[3] == PCM_BYTES);
    /* overflowed() is false here, so only the 40 ms successful-read guard
     * detects this loss; it must start a new capture segment. */
    assert(packets[2].pts == 1033);
    samples(2, 0, PCM_BYTES, 0x44);
}

static void generation_and_clock_wrap(void)
{
    setup();
    s_generation = UINT32_MAX-1;
    uint64_t base = (uint64_t)UINT32_MAX-9;
    add(base, PCM_BYTES, 0x11);
    add(base+10, PCM_BYTES/2, 0x22);
    add(base+25, PCM_BYTES, 0x33);
    events[2].new_generation = true;
    add(base+45, PCM_BYTES, 0x44);
    events[3].new_generation = true;
    add(base+65, PCM_BYTES, 0x55);
    run();
    assert(packet_count == 4 && codec_resets == 3);
    assert(packets[0].pts == (uint32_t)(base-20));
    assert(packets[1].pts == (uint32_t)(base+5));
    assert(packets[2].pts == (uint32_t)(base+25));
    assert(packets[3].pts == (uint32_t)(base+45));
    assert(packets[1].generation == UINT32_MAX && packets[2].generation == 0);
    samples(1, 0, PCM_BYTES, 0x33);
    samples(2, 0, PCM_BYTES, 0x44);
    samples(3, 0, PCM_BYTES, 0x55);

    setup();
    for (unsigned i=0; i<8; ++i) add(base+i*20, PCM_BYTES, (uint8_t)i);
    run();
    assert(packet_count == 8 && codec_resets == 1);
    for (unsigned i=0; i<8; ++i) {
        assert(packets[i].pts == (uint32_t)(base-20+i*20));
        samples(i, 0, PCM_BYTES, (uint8_t)i);
    }
}

static void negative_drift_recovery(bool overflow, bool wrap)
{
    setup();
    const unsigned frames = 5000;
    /* Just before timer wrap, the sample-derived next PTS has already wrapped
     * while the stale wall-clock candidate has not. Numeric max() is wrong. */
    uint64_t base = wrap ? (uint64_t)UINT32_MAX-99920 : 1000;
    for (unsigned i=0; i<frames; ++i)
        add(base+i*20-i/50, PCM_BYTES, (uint8_t)i);
    uint64_t last = events[event_count-1].now_ms;
    unsigned recovered_event;
    if (overflow) {
        add(last+10, PCM_BYTES/2, 0x22);
        add(last+30, 8000, 0x33);
        events[event_count-1].racing_bytes = 384;
        events[event_count-1].racing_value = 0x44;
        recovered_event = event_count;
        add(last+40, 0, 0);
        add(last+45, 0, 0); /* Drain remaining FIFO bytes into an old partial. */
        add(last+60, PCM_BYTES, 0x55);
    } else {
        recovered_event = event_count;
        add(last+41, PCM_BYTES, 0x33);
        add(last+51, PCM_BYTES/2, 0x22);
        add(last+61, PCM_BYTES, 0x55);
    }
    events[event_count-1].new_generation = true;
    run();
    assert(packet_count == frames+2 && codec_resets == 2);
    for (unsigned i=0; i<frames; ++i)
        assert(packets[i].pts == (uint32_t)(base-20+i*20));
    assert(packets[frames].event == recovered_event);
    /* A real capture discontinuity cannot move this session's sample clock
     * backwards after accumulated negative drift. No old partial may leak. */
    assert((uint32_t)(packets[frames].pts-packets[frames-1].pts) == 20);
    samples(frames, 0, PCM_BYTES, 0x33);
    /* A new generation deliberately resets the bound and uses the fresh
     * wall-clock anchor, even when it is numerically/temporally earlier. */
    assert(packets[frames+1].generation == 11);
    assert(packets[frames+1].pts == (uint32_t)(last+(overflow ? 40 : 41)));
    samples(frames+1, 0, PCM_BYTES, 0x55);
}

int main(void)
{
    downmix_levels();
    clock_drift(1);
    clock_drift(-1);
    overflow_during_read();
    overflow_already_visible();
    pause_discards_partial(false);
    pause_discards_partial(true);
    forty_ms_continuity();
    double_overflow_hidden_by_fifo();
    generation_and_clock_wrap();
    negative_drift_recovery(false, false);
    negative_drift_recovery(true, false);
    negative_drift_recovery(false, true);
    negative_drift_recovery(true, true);
    puts("Audio capture: production worker + TinyUSB FIFO, drift, overflow races, partial discard, monotonic recovery and wrap: PASS");
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "tusb_config.h").write_text(
        "#define CFG_TUSB_MCU OPT_MCU_NONE\n"
        "#define CFG_TUSB_OS OPT_OS_NONE\n"
        "#define CFG_TUD_ENABLED 0\n"
        "#define CFG_TUH_ENABLED 0\n"
    )
    production = "".join(function(name) for name in (
        "capture_pts", "capture_discontinuous", "capture_read_skipped",
        "downmix_to_mono", "audio_task"
    ))
    (path / "test.c").write_text(PREFIX + production + MAIN)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I" + str(path), "-I" + str(TINYUSB),
        str(path / "test.c"), str(TINYUSB / "common/tusb_fifo.c"),
        "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)
