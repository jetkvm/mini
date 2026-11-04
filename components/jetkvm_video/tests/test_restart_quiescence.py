#!/usr/bin/env python3
"""Run actual CSI restart/reset code with an encoder surviving a failed pause."""

from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / "jetkvm_video.c").read_text()


def function(signature):
    start = source.index(signature)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


reset = function("static void csi_reset_buffer_state(app_ctx_t *ctx)\n{")
restart = function("static esp_err_t csi_restart_rx(app_ctx_t *ctx)\n{")

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NOT_ALLOWED 3
#define DIRECT_H264_DRAIN_MS 3000
#define CAM_CTLR_COLOR_YUV420 1
#define TC_CSI_OUTPUT_YUV_COLOR CAM_CTLR_COLOR_YUV420
#define CSI_BUF_AFULL_THRD_OVERRIDE 0
#define CSI_DMA_BURST_LEN_OVERRIDE 0
#define pdTRUE 1
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define ESP_RETURN_ON_FALSE(condition, error, ...) do { \
    if (!(condition)) return (error); \
} while (0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)

typedef struct { bool output_rgb888; } candidate_t;
typedef struct {
    void *cam;
    bool direct_h264_active;
    int direct_h264_encoding_index;
    struct { candidate_t *active_candidate; } bridge;
    unsigned capture_width, capture_height;
    unsigned frames_finished, frames_consumed;
    size_t last_received_size;
    void *frame_buffer, *capture_buffers[4];
    unsigned capture_buffer_count, capture_next_buffer;
    uint32_t free_buffers, inflight_buffers, ready_buffers, locked_buffers;
    unsigned csi_no_free_buffers;
    size_t capture_received_sizes[4];
    uint32_t capture_buffer_sequences[4];
    int64_t capture_buffer_timestamps_us[4];
} app_ctx_t;
static int s_direct_h264_pipeline_mux, camera_token;
static unsigned char buffers[4][64];
typedef struct {
    struct { uint32_t csi_brg_rst; } csi_en;
    struct { uint32_t csi_host_cm_en; } host_cm_ctrl;
    struct { uint32_t csi_host_cm_vnum, csi_host_cm_hnum; } host_size_ctrl;
    struct { uint32_t val; } int_clr, int_ena;
} bridge_registers_t;
static bridge_registers_t MIPI_CSI_BRIDGE;
static struct {
    int stop_result, drain_result;
    unsigned stops, drains, starts, deactivations, activations, delays;
    bool producer_stopped, encoder_owned;
} calls;

static uint32_t csi_all_buffer_mask(const app_ctx_t *ctx)
{
    return (1u << ctx->capture_buffer_count) - 1;
}

static void direct_h264_pipeline_deactivate(app_ctx_t *ctx)
{
    ++calls.deactivations;
    ctx->direct_h264_active = false;
    /* Deactivation prevents new jobs; an already encoding job keeps its raw. */
}

static void direct_h264_pipeline_activate(app_ctx_t *ctx)
{
    assert(!calls.encoder_owned && calls.starts != 0);
    ++calls.activations;
    ctx->direct_h264_active = true;
}

static int esp_cam_ctlr_stop(void *cam)
{
    assert(cam == &camera_token);
    ++calls.stops;
    if (calls.stop_result == ESP_OK || calls.stop_result == ESP_ERR_INVALID_STATE) {
        calls.producer_stopped = true;
    }
    return calls.stop_result;
}

static int direct_h264_pipeline_drain(app_ctx_t *ctx, unsigned timeout)
{
    assert(calls.producer_stopped && !ctx->direct_h264_active);
    assert(timeout == DIRECT_H264_DRAIN_MS);
    ++calls.drains;
    if (calls.drain_result == ESP_OK) {
        calls.encoder_owned = false;
        ctx->direct_h264_encoding_index = -1;
    }
    return calls.drain_result;
}

static void esp_rom_delay_us(unsigned delay)
{
    assert(delay == 2 && calls.drains != 0 && !calls.encoder_owned);
    ++calls.delays;
}

static unsigned ulTaskNotifyTake(int clear, unsigned timeout)
{
    assert(clear == pdTRUE && timeout == 0 && !calls.encoder_owned);
    return 0;
}

static int esp_cam_ctlr_start(void *cam)
{
    assert(cam == &camera_token && calls.producer_stopped);
    assert(calls.drains != 0 && !calls.encoder_owned);
    ++calls.starts;
    return ESP_OK;
}

/* PRODUCTION */

static app_ctx_t initial(bool active)
{
    memset(&calls, 0, sizeof(calls));
    calls.encoder_owned = true;
    memset(&MIPI_CSI_BRIDGE, 0xa5, sizeof(MIPI_CSI_BRIDGE));
    app_ctx_t ctx = {
        .cam = &camera_token, .direct_h264_active = active,
        .direct_h264_encoding_index = 3,
        .capture_width = 1920, .capture_height = 1080,
        .frames_finished = 40, .frames_consumed = 39,
        .last_received_size = 64, .frame_buffer = buffers[3],
        .capture_buffer_count = 4, .capture_next_buffer = 2,
        .free_buffers = 1, .inflight_buffers = 6, .locked_buffers = 8,
        .csi_no_free_buffers = 7,
    };
    for (unsigned i = 0; i < 4; ++i) {
        ctx.capture_buffers[i] = buffers[i];
        ctx.capture_received_sizes[i] = 64;
        ctx.capture_buffer_sequences[i] = 40 + i;
        ctx.capture_buffer_timestamps_us[i] = 1000000 + i;
    }
    return ctx;
}

static void assert_preserved(app_ctx_t *ctx, app_ctx_t before,
                              bridge_registers_t registers_before)
{
    before.direct_h264_active = false; /* The only permitted state change. */
    assert(memcmp(ctx, &before, sizeof(before)) == 0);
    assert(memcmp(&MIPI_CSI_BRIDGE, &registers_before, sizeof(registers_before)) == 0);
    assert(calls.encoder_owned && calls.starts == 0 && calls.delays == 0);
}

static void assert_restarted(const app_ctx_t *ctx)
{
    assert(calls.starts == 1 && calls.drains != 0 && !calls.encoder_owned);
    assert(ctx->direct_h264_encoding_index == -1);
    assert(ctx->free_buffers == 15 && ctx->locked_buffers == 0);
    assert(ctx->inflight_buffers == 0 && ctx->ready_buffers == 0);
    assert(ctx->frames_finished == 0 && ctx->frames_consumed == 0);
    assert(ctx->last_received_size == 0 && ctx->frame_buffer == buffers[0]);
    for (unsigned i = 0; i < 4; ++i) {
        assert(ctx->capture_received_sizes[i] == 0);
        assert(ctx->capture_buffer_sequences[i] == 0);
        assert(ctx->capture_buffer_timestamps_us[i] == 0);
    }
}

int main(void)
{
    /* Sleep rollback enters with active=false even though an old encode has
     * not finished. This must drain before bridge/raw reset and CSI start. */
    app_ctx_t ctx = initial(false);
    assert(csi_restart_rx(&ctx) == ESP_OK);
    assert_restarted(&ctx);
    assert(calls.drains == 1 && calls.activations == 0 && !ctx.direct_h264_active);

    /* A timed-out drain must leave raw ownership and bridge state intact. */
    ctx = initial(false);
    calls.drain_result = ESP_ERR_TIMEOUT;
    app_ctx_t before = ctx;
    bridge_registers_t registers_before = MIPI_CSI_BRIDGE;
    assert(csi_restart_rx(&ctx) == ESP_ERR_TIMEOUT);
    assert_preserved(&ctx, before, registers_before);
    assert(calls.drains == 1);

    /* First stop fails after deactivation. The retry must still drain even
     * though direct_h264_active is now false. A later success may rearm. */
    ctx = initial(true);
    before = ctx;
    registers_before = MIPI_CSI_BRIDGE;
    calls.stop_result = ESP_ERR_TIMEOUT;
    assert(csi_restart_rx(&ctx) == ESP_ERR_TIMEOUT);
    assert_preserved(&ctx, before, registers_before);
    assert(calls.deactivations == 1 && calls.drains == 0);
    calls.stop_result = ESP_OK;
    calls.drain_result = ESP_ERR_TIMEOUT;
    assert(csi_restart_rx(&ctx) == ESP_ERR_TIMEOUT);
    assert_preserved(&ctx, before, registers_before);
    assert(calls.drains == 1);
    calls.drain_result = ESP_OK;
    assert(csi_restart_rx(&ctx) == ESP_OK);
    assert_restarted(&ctx);
    assert(calls.drains == 2 && calls.activations == 0);

    /* The ordinary active restart still reactivates only after draining. */
    ctx = initial(true);
    assert(csi_restart_rx(&ctx) == ESP_OK);
    assert_restarted(&ctx);
    assert(calls.deactivations == 1 && calls.activations == 1 && ctx.direct_h264_active);

    /* An already stopped controller does not imply a drained encoder. */
    ctx = initial(false);
    calls.stop_result = ESP_ERR_INVALID_STATE;
    assert(csi_restart_rx(&ctx) == ESP_OK);
    assert_restarted(&ctx);
    assert(calls.drains == 1);
    puts("CSI restart: inactive encoding owner, failed stop/drain, retry ordering: PASS");
}
"""

with tempfile.TemporaryDirectory(prefix="csi-restart-quiescence-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(HARNESS.replace("/* PRODUCTION */", reset + "\n" + restart))
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         str(path / "test.c"), "-o", str(path / "test")], check=True,
    )
    subprocess.run([str(path / "test")], check=True)
