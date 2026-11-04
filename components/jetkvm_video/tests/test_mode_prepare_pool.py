#!/usr/bin/env python3
"""A host resolution change keeps every video pool buffer.

The capture slots are allocated once at boot. video_mode_prepare() runs on
every mode change after csi_destroy(); it must not free a slot, and it must
report that the rebuild allocates no PSRAM, for 720p60 -> 1080p30 -> 720p60.

This compiles the real video_mode_prepare() and the helpers it calls from
jetkvm_video.c against a minimal context.
"""

from pathlib import Path
import re
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
SOURCE = (COMPONENT / 'jetkvm_video.c').read_text()
DRIVER_HEADER = (COMPONENT.parent / 'p4_csi_capture/include/p4_csi_capture.h').read_text()


def function(name):
    match = re.search(r'^static [^;{]*?\b' + name + r'\([^;]*?\)\s*\{', SOURCE, re.M | re.S)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end]


def define(name):
    match = re.search(rf'^#define {name}\b(?:.*\\\n)*.*\n', SOURCE, re.M)
    assert match, name
    return match.group(0)


functions = [function(name) for name in (
    'csi_effective_output_yuv_color', 'csi_output_matches_h264_raw',
    'csi_use_compact_ring', 'h264_mode_fps', 'h264_hw_matches',
    'h264_hw_deinit', 'video_mode_prepare',
)]
macros = [define(name) for name in (
    'TC_CSI_INPUT_8BIT_SWAP', 'TC_CSI_INPUT_16BIT_SWAP',
    'TC_CSI_OUTPUT_YUV_COLOR', 'TC_H264_RAW_FORMAT',
    'FRAME_WIDTH_MAX', 'FRAME_HEIGHT_MAX', 'CSI_FRAME_BUFFER_COUNT',
    'CSI_FRAME_BUFFER_COUNT_DEFAULT', 'CSI_FRAME_BUFFER_COUNT_DIRECT_1080P30',
    'CSI_FRAME_BUFFER_COUNT_COMPACT_1080P30',
    'CSI_FRAME_BUFFER_COUNT_DIRECT_HIGH_RATE', 'DIRECT_H264_OVERLAP_ENABLE',
    'HDMI_DEBUG_CAPTURE_ACTIVE_DE', 'H264_OUTPUT_MIN_BYTES',
    'CSI_DMA_BURST_LEN_OVERRIDE', 'CSI_DMA_BURST_BYTES', 'CSI_CAPTURE_CAPACITY',
)]

backup_align = re.search(r'^#define P4_CSI_BACKUP_BUFFER_ALIGN_BYTES .*\n', DRIVER_HEADER, re.M)
assert backup_align
macros.append(backup_align.group(0))

prefix = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_SUPPORTED 0x106
typedef enum {
    CAM_CTLR_COLOR_RGB888, CAM_CTLR_COLOR_YUV420, CAM_CTLR_COLOR_YUV422_UYVY,
    CAM_CTLR_COLOR_YUV422_YUYV, CAM_CTLR_COLOR_YUV422_VYUY,
    CAM_CTLR_COLOR_YUV422_YVYU,
} cam_ctlr_color_t;
typedef enum {
    ESP_H264_RAW_FMT_O_UYY_E_VYY, ESP_H264_RAW_FMT_UYVY, ESP_H264_RAW_FMT_YUYV,
    ESP_H264_RAW_FMT_BGR888,
} esp_h264_raw_format_t;
typedef void *esp_h264_enc_handle_t;
typedef struct {
    uint16_t width, height, frame_width, frame_height, fps;
} tc358743_timings_t;
typedef struct {
    bool use_full_timing, output_rgb888;
} tc358743_capture_candidate_t;
'''

context = r'''
typedef struct {
    bool preview_only_active;
    bool use_driver_backup;
    size_t capture_buffers_capacity;
    void *capture_buffers[CSI_FRAME_BUFFER_COUNT];
    esp_h264_enc_handle_t h264_enc;
    uint8_t *h264_output_buf;
    uint32_t h264_output_len;
    uint16_t h264_width, h264_height;
    uint8_t h264_fps, h264_gop;
    uint32_t h264_bitrate;
    esp_h264_raw_format_t h264_pic_type;
} app_ctx_t;

static char pool[CSI_FRAME_BUFFER_COUNT];
static int pool_frees;

static bool in_pool(const void *pointer) {
    return pointer >= (const void *)pool &&
           pointer < (const void *)(pool + CSI_FRAME_BUFFER_COUNT);
}
static void heap_caps_free(void *pointer) {
    if (in_pool(pointer)) {
        printf("FAIL: capture slot %d freed\n", (int)((char *)pointer - pool));
        pool_frees++;
    }
}
static void h264_sync_idr_production_complete(app_ctx_t *ctx, bool success) {
    (void)ctx; (void)success;
}
static void esp_h264_enc_close(esp_h264_enc_handle_t enc) { (void)enc; }
static void esp_h264_enc_del(esp_h264_enc_handle_t enc) { (void)enc; }
static size_t h264_raw_frame_size(uint16_t width, uint16_t height,
                                  esp_h264_raw_format_t fmt) {
    (void)fmt; return (size_t)width * height * 3U / 2U;
}
static size_t h264_hw_database_bytes(uint16_t width, uint16_t height) {
    const size_t mb = ((width + 15U) / 16U) * ((height + 15U) / 16U);
    return mb * 384U + 16U;
}
'''

cases = r'''
static const tc358743_capture_candidate_t yuv = {0};
static const tc358743_timings_t m720p60 = {1280, 720, 1650, 750, 60};
static const tc358743_timings_t m1080p30 = {1920, 1080, 2200, 1125, 30};

static int failures;
static app_ctx_t ctx;
static uint8_t encoder;

static void change_to(const tc358743_timings_t *mode, const char *name) {
    size_t bytes = SIZE_MAX;
    esp_err_t ret = video_mode_prepare(&ctx, mode, &yuv, &bytes);
    if (ret != ESP_OK) {
        printf("FAIL: %s: video_mode_prepare returned 0x%x\n", name, ret);
        failures++;
        return;
    }
    for (int i = 0; i < CSI_FRAME_BUFFER_COUNT; i++) {
        if (ctx.capture_buffers[i] != &pool[i]) {
            printf("FAIL: %s: slot %d no longer holds its pool buffer\n", name, i);
            failures++;
        }
    }
    if (bytes != 0U) {
        printf("FAIL: %s: rebuild still allocates %zu bytes of PSRAM\n", name, bytes);
        failures++;
    } else {
        printf("ok:   %s: all %d slots kept, rebuild allocates 0 bytes\n",
               name, CSI_FRAME_BUFFER_COUNT);
    }
    /* tc_start_streaming() then creates the encoder for this mode. */
    ctx.h264_enc = &encoder;
    ctx.h264_width = mode->width;
    ctx.h264_height = mode->height;
    ctx.h264_fps = h264_mode_fps(mode->width, mode->height, mode->fps);
    ctx.h264_pic_type = TC_H264_RAW_FORMAT;
}

int main(void) {
    /* Boot state: every slot of the pool is present. */
    for (int i = 0; i < CSI_FRAME_BUFFER_COUNT; i++)
        ctx.capture_buffers[i] = &pool[i];
    ctx.capture_buffers_capacity = (size_t)FRAME_WIDTH_MAX * FRAME_HEIGHT_MAX * 3U / 2U;

    change_to(&m720p60, "boot at 720p60");
    change_to(&m1080p30, "720p60 -> 1080p30");
    change_to(&m720p60, "1080p30 -> 720p60");
    change_to(&m1080p30, "720p60 -> 1080p30 again");
    /* With the driver's backup buffer on, csi_init() makes the CSI driver
     * allocate one frame in PSRAM, and the rebuild reports it. */
    ctx.use_driver_backup = true;
    size_t bytes = 0U;
    if (video_mode_prepare(&ctx, &m1080p30, &yuv, &bytes) != ESP_OK ||
        bytes != 1920U * 1080U * 2U) {
        printf("FAIL: backup buffer: rebuild reports %zu bytes, not %u\n",
               bytes, 1920U * 1080U * 2U);
        failures++;
    } else {
        printf("ok:   backup buffer: rebuild reports its %zu bytes\n", bytes);
    }
    failures += pool_frees;
    if (failures) {
        printf("mode prepare with the video pool: %d FAILED\n", failures);
        return 1;
    }
    puts("mode prepare with the video pool: no slot freed, no PSRAM requested: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='p4-mode-prepare-pool-') as temporary:
    directory = Path(temporary)
    source = directory / 'test.c'
    executable = directory / 'test'
    source.write_text('\n'.join([prefix, *macros, context, *functions, cases]))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-parameter',
                    str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
