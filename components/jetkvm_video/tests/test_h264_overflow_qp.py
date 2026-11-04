#!/usr/bin/env python3
"""An access unit that overflows the output buffer raises the encoder's QP.

The vendor rate control (h264_rc.c, as patched) is compiled unchanged and
driven by a model of the encoder loop in esp_h264_enc_single_hw.c: an IDR is
coded at the frame QP that rate control picks, and its size halves for every
six QP steps. A static screen keeps P frames small, so rate control lowers the
QP to its minimum; a detailed desktop then makes the IDR at that QP larger than
the output buffer. Without overflow feedback every following IDR is coded at
the same QP and overflows again, so video stops for good (for example with a
photo as the desktop wallpaper). With the feedback the next IDR fits and
later ones keep fitting, and the QP returns to its minimum once the screen is
simple again.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
VENDOR = ROOT / "managed_components/espressif__esp_h264/hw/src"
rc_source = (VENDOR / "h264_rc.c").read_text()
rc_header = (VENDOR / "h264_rc.h").read_text()
encoder = (VENDOR / "esp_h264_enc_single_hw.c").read_text()


def block(source, marker):
    start = source.index(marker)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


# The encoder reports both overflow kinds to rate control: ERR_MEM, the stall
# that ends in the frame timeout with the bitstream DMA out of descriptors,
# and ERR_OVERFLOW, the overflow flag or length check after FRAME_DONE. A
# frame that fits reports its size. Both happen under the parameter mutex
# that esp_h264_enc_set_bitrate() also takes.
process = block(encoder, "static esp_h264_err_t enc_process(")
locked = process[process.index("esp_h264_mutex_lock(mutex"):process.index("esp_h264_mutex_unlock(mutex)")]
assert locked.index("h264_hw_enc_gop_mode_process(") < locked.index("esp_h264_rc_overflow(")
assert re.search(r"ret == ESP_H264_ERR_MEM \|\| ret == ESP_H264_ERR_OVERFLOW\)\) \{\s*"
                 r"uint8_t qp_min = esp_h264_rc_overflow\(rc_hd\);", locked), locked
assert re.search(r"ret == ESP_H264_ERR_OK\) \{\s*esp_h264_rc_frame_fit\(rc_hd, "
                 r"out_frame->frame_type == ESP_H264_FRAME_TYPE_IDR,\s*"
                 r"out_frame->length, out_frame->raw_data.len\);", locked), locked

ALLOC = r'''
#pragma once
#include <stdint.h>
#include <stdlib.h>
#define ESP_H264_MEM_INTERNAL 1
#define ESP_H264_MEM_SPIRAM 2
static inline void *esp_h264_calloc_prefer(uint32_t n, uint32_t size, uint32_t *actual,
                                           uint32_t caps1, uint32_t caps2)
{
    (void)caps1; (void)caps2;
    *actual = n * size;
    return calloc(n, size);
}
static inline void esp_h264_free(void *p) { free(p); }
'''

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "h264_rc.h"

#define MB_W 120
#define MB_H 68
#define MBS (MB_W * MB_H)
#define CAP (512U * 1024U)          /* H264_OUTPUT_MIN_BYTES, the 1080p slot */
#define QP_MIN 28                   /* H264_QUALITY_QP_MIN */
#define QP_MAX 51
#define BITRATE 7000000U            /* 1080p30 High */
#define FPS 30
#define P_BYTES 1500U               /* static screen, cursor moves */

typedef struct {
    esp_h264_rc_hd_t rc;
    int gop;
    int frame_num;
    int feedback;                   /* report overflows and fits to RC */
    int overflows, idrs, last_overflow, last_idr_qp, min_qp_window;
    uint32_t max_idr_bytes;
} enc_t;

/* IDR size at QP 28 for the screen; it halves every 6 QP. */
static uint32_t idr_bytes(double at_qp_min, uint8_t qp)
{
    return (uint32_t)(at_qp_min * pow(2.0, -((double)qp - QP_MIN) / 6.0));
}

/* One frame as h264_hw_enc_gop_mode_process() and enc_process() handle it. */
static void encode(enc_t *e, int index, double screen)
{
    bool idr = e->frame_num % e->gop == 0;
    if (idr) e->frame_num = 0;
    uint32_t rate, pred_mad;
    uint8_t qp;
    esp_h264_rc_start(e->rc, e->frame_num == 0, &rate, &pred_mad, &qp);
    uint32_t bytes = idr ? idr_bytes(screen, qp) : P_BYTES;
    if (qp < e->min_qp_window) e->min_qp_window = qp;
    if (bytes > CAP) {
        if (e->feedback) esp_h264_rc_overflow(e->rc);
        e->overflows++;
        e->last_overflow = index;
        e->frame_num = 0;           /* the next frame is an IDR */
        return;
    }
    esp_h264_rc_end(e->rc, bytes * 8U, (uint32_t)qp * MBS, 4U * MBS);
    if (e->feedback) esp_h264_rc_frame_fit(e->rc, e->frame_num == 0, bytes, CAP);
    if (idr) {
        e->idrs++;
        e->last_idr_qp = qp;
        if (bytes > e->max_idr_bytes) e->max_idr_bytes = bytes;
    }
    e->frame_num++;
}

static void run(enc_t *e, int *index, int frames, double screen)
{
    e->min_qp_window = QP_MAX + 1;
    for (int i = 0; i < frames; i++, (*index)++) encode(e, *index, screen);
}

static enc_t start(int gop, int feedback)
{
    enc_t e = {0};
    e.rc = esp_h264_enc_hw_rc_new(QP_MAX, QP_MIN, BITRATE, FPS, MB_W, MB_H);
    assert(e.rc);
    e.gop = gop;
    e.feedback = feedback;
    e.last_overflow = -1;
    return e;
}

int main(void)
{
    const int gops[] = {30, 15};    /* Wi-Fi: 1 IDR/s, Ethernet: 2 IDR/s */
    for (unsigned g = 0; g < sizeof(gops) / sizeof(gops[0]); g++) {
        int gop = gops[g];

        /* Without feedback: a simple window, then the photo wallpaper (an IDR
         * of 700 KiB at QP 28). Every IDR from then on overflows. */
        enc_t old = start(gop, 0);
        int i = 0;
        run(&old, &i, 20 * FPS, 100.0 * 1024);
        assert(old.overflows == 0);
        assert(old.min_qp_window == QP_MIN);
        int idrs_before = old.idrs;
        run(&old, &i, 30 * FPS, 700.0 * 1024);
        assert(old.idrs == idrs_before);           /* no picture in 30 s */
        assert(old.overflows == 30 * FPS);         /* every frame an overflowing IDR */
        esp_h264_enc_hw_rc_del(old.rc);

        /* With feedback, the same screens. */
        enc_t e = start(gop, 1);
        i = 0;
        run(&e, &i, 20 * FPS, 100.0 * 1024);
        assert(e.overflows == 0);
        assert(e.min_qp_window == QP_MIN);         /* no cost on a simple screen */
        int at = i;
        run(&e, &i, 30 * FPS, 700.0 * 1024);
        assert(e.overflows == 1);                  /* the first IDR only */
        assert(e.last_overflow - at < 2 * gop);
        assert(e.max_idr_bytes <= CAP);
        assert(e.last_idr_qp > QP_MIN && e.last_idr_qp <= QP_MIN + 12);
        printf("gop %d: wallpaper IDR at QP %d, %u bytes of %u\n",
               gop, e.last_idr_qp, e.max_idr_bytes, CAP);

        /* A screen far over the buffer at QP 28 (1.5 MiB) needs two raises. */
        e.max_idr_bytes = 0;
        int before = e.overflows;
        at = i;
        run(&e, &i, 30 * FPS, 1536.0 * 1024);
        assert(e.overflows - before <= 2);
        assert(e.last_overflow - at < 3 * gop);
        assert(e.max_idr_bytes <= CAP);

        /* Back to a simple screen: QP returns to its minimum. */
        run(&e, &i, 60 * FPS, 100.0 * 1024);
        assert(e.overflows - before <= 2);
        run(&e, &i, 2 * FPS, 100.0 * 1024);
        assert(e.min_qp_window == QP_MIN);
        esp_h264_enc_hw_rc_del(e.rc);
    }
    printf("H.264 overflow raises QP, recovers in one GOP, relaxes on simple screens: PASS\n");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="h264-overflow-qp-") as directory:
    path = Path(directory)
    (path / "esp_h264_alloc.h").write_text(ALLOC)
    (path / "h264_rc.h").write_text(rc_header)
    (path / "h264_rc.c").write_text(rc_source)
    (path / "test.c").write_text(HARNESS)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(path), str(path / "h264_rc.c"), str(path / "test.c"),
        "-lm", "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)
