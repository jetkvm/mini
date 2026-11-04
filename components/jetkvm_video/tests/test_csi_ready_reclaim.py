#!/usr/bin/env python3
"""Run the production CSI allocator against host-side ownership scenarios."""

from pathlib import Path
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
source = (COMPONENT / "jetkvm_video.c").read_text()
start = source.index("static bool IRAM_ATTR csi_get_new_trans(")
end = source.index("static bool IRAM_ATTR csi_trans_started(", start)
allocator = source[start:end]

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IRAM_ATTR
#define DIRECT_H264_INVALID_INDEX (-1)
typedef void *esp_cam_ctlr_handle_t;
typedef struct { void *buffer; size_t buflen; } esp_cam_ctlr_trans_t;
typedef struct {
    uint32_t generation;
    bool capture_complete, cancelled, overlap_candidate;
} direct_h264_slot_t;
typedef struct {
    bool use_driver_backup;
    uint8_t capture_buffer_count, capture_next_buffer;
    uint32_t free_buffers, inflight_buffers, ready_buffers, locked_buffers;
    uint32_t csi_no_free_buffers;
    bool direct_h264_active, direct_h264_overlap_enabled;
    int8_t direct_h264_ready_index, direct_h264_encoding_index;
    uint32_t direct_h264_generation;
    direct_h264_slot_t direct_h264_slots[6];
    size_t capture_received_sizes[6];
    uint32_t capture_buffer_sequences[6];
    int64_t capture_buffer_timestamps_us[6];
    void *capture_buffers[6];
    size_t capture_buffer_capacity;
    struct { uint32_t capture_nofree_drops, direct_ready_replaced; } metrics;
} app_ctx_t;

static int s_direct_h264_pipeline_mux, s_csi_buffer_mux, lock_depth;
static void enter_lock(int *mux)
{
    assert((lock_depth == 0 && mux == &s_direct_h264_pipeline_mux) ||
           (lock_depth == 1 && mux == &s_csi_buffer_mux));
    ++lock_depth;
}
static void exit_lock(int *mux)
{
    assert((lock_depth == 2 && mux == &s_csi_buffer_mux) ||
           (lock_depth == 1 && mux == &s_direct_h264_pipeline_mux));
    --lock_depth;
}
#define portENTER_CRITICAL_ISR(mux) enter_lock(mux)
#define portEXIT_CRITICAL_ISR(mux) exit_lock(mux)
static uint32_t csi_all_buffer_mask(const app_ctx_t *ctx)
{
    return (1u << ctx->capture_buffer_count) - 1;
}

/* PRODUCTION_ALLOCATOR */

static app_ctx_t saturated(void)
{
    static uint8_t buffers[6];
    app_ctx_t ctx = {0};
    ctx.capture_buffer_count = 5;
    ctx.capture_buffer_capacity = 3110400;
    ctx.inflight_buffers = 0x07; /* Three descriptors, including this EOF. */
    ctx.locked_buffers = 0x18;   /* Encoding 3, completed READY 4. */
    ctx.direct_h264_active = true;
    ctx.direct_h264_encoding_index = 3;
    ctx.direct_h264_ready_index = 4;
    ctx.direct_h264_generation = 7;
    ctx.direct_h264_slots[4].generation = 7;
    ctx.direct_h264_slots[4].capture_complete = true;
    ctx.capture_received_sizes[4] = ctx.capture_buffer_capacity;
    ctx.capture_buffer_sequences[4] = 41;
    ctx.capture_buffer_timestamps_us[4] = 1000000;
    for (int i = 0; i < 6; ++i) {
        ctx.capture_buffers[i] = &buffers[i];
    }
    return ctx;
}

static esp_cam_ctlr_trans_t request(app_ctx_t *ctx)
{
    esp_cam_ctlr_trans_t trans = {0};
    assert(!csi_get_new_trans(NULL, &trans, ctx));
    assert(lock_depth == 0);
    return trans;
}

static void expect_unavailable(app_ctx_t ctx)
{
    app_ctx_t before = ctx;
    esp_cam_ctlr_trans_t trans = request(&ctx);
    assert(trans.buffer == NULL && trans.buflen == 0);
    /* A rejected reclaim must not alter ownership or the pending job. */
    ++before.csi_no_free_buffers;
    ++before.metrics.capture_nofree_drops;
    assert(memcmp(&ctx, &before, sizeof(ctx)) == 0);
}

int main(void)
{
    app_ctx_t ctx = saturated();
    esp_cam_ctlr_trans_t trans = request(&ctx);
    assert(trans.buffer == ctx.capture_buffers[4]);
    assert(trans.buflen == ctx.capture_buffer_capacity);
    assert(ctx.direct_h264_ready_index == -1);
    assert(ctx.direct_h264_encoding_index == 3);
    assert(ctx.direct_h264_slots[4].cancelled);
    assert(ctx.inflight_buffers == 0x17 && ctx.locked_buffers == 0x08);
    assert(ctx.free_buffers == 0 && ctx.ready_buffers == 0);
    assert(ctx.capture_received_sizes[4] == 0);
    assert(ctx.capture_buffer_sequences[4] == 0);
    assert(ctx.capture_buffer_timestamps_us[4] == 0);
    assert(ctx.metrics.direct_ready_replaced == 1);
    assert(ctx.metrics.capture_nofree_drops == 0);
    assert(ctx.csi_no_free_buffers == 0);
    /* Until the real EOF releases a descriptor, there is no second victim. */
    expect_unavailable(ctx);

    /* Spare capacity must preserve READY and the ordinary rotation policy. */
    ctx = saturated();
    ctx.capture_buffer_count = 6;
    ctx.free_buffers = 1u << 5;
    ctx.capture_next_buffer = 4;
    trans = request(&ctx);
    assert(trans.buffer == ctx.capture_buffers[5]);
    assert(ctx.direct_h264_ready_index == 4);
    assert(!ctx.direct_h264_slots[4].cancelled);
    assert(ctx.locked_buffers == 0x18 && ctx.inflight_buffers == 0x27);
    assert(ctx.metrics.direct_ready_replaced == 0);

    /* Teardown/generation changes, overlap, incomplete data and ownership
     * inconsistencies must all fail closed without touching the candidate. */
    ctx = saturated(); ctx.direct_h264_active = false; expect_unavailable(ctx);
    ctx = saturated(); ctx.direct_h264_overlap_enabled = true; expect_unavailable(ctx);
    ctx = saturated(); ctx.direct_h264_slots[4].generation--; expect_unavailable(ctx);
    ctx = saturated(); ctx.direct_h264_slots[4].capture_complete = false; expect_unavailable(ctx);
    ctx = saturated(); ctx.direct_h264_slots[4].cancelled = true; expect_unavailable(ctx);
    ctx = saturated(); ctx.direct_h264_slots[4].overlap_candidate = true; expect_unavailable(ctx);
    ctx = saturated(); ctx.direct_h264_ready_index = -1; expect_unavailable(ctx);
    ctx = saturated(); ctx.direct_h264_ready_index = 5; expect_unavailable(ctx);
    ctx = saturated(); ctx.direct_h264_encoding_index = 4; expect_unavailable(ctx);
    ctx = saturated(); ctx.locked_buffers &= ~(1u << 4); expect_unavailable(ctx);
    ctx = saturated(); ctx.inflight_buffers |= 1u << 4; expect_unavailable(ctx);
    ctx = saturated(); ctx.ready_buffers |= 1u << 4; expect_unavailable(ctx);
    ctx = saturated(); ctx.free_buffers |= 1u << 4; expect_unavailable(ctx);

    /* Every still-armed descriptor is protected, even with stale READY
     * metadata claiming it. The actual encoding buffer is protected too. */
    for (int8_t i = 0; i < 4; ++i) {
        ctx = saturated();
        ctx.direct_h264_ready_index = i;
        ctx.direct_h264_slots[i] = ctx.direct_h264_slots[4];
        expect_unavailable(ctx);
    }

    ctx = saturated();
    ctx.use_driver_backup = true;
    trans = request(&ctx);
    assert(trans.buffer == NULL && trans.buflen == 0);
    assert(ctx.metrics.capture_nofree_drops == 0);
    assert(ctx.metrics.direct_ready_replaced == 0);
    ctx = saturated();
    ctx.capture_buffer_count = 0;
    trans = request(&ctx);
    assert(trans.buffer == NULL && trans.buflen == 0);
    assert(ctx.metrics.capture_nofree_drops == 0);
    puts("CSI READY reclamation ownership and lock-order scenarios: PASS");
}
"""

with tempfile.TemporaryDirectory(prefix="csi-ready-reclaim-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(
        HARNESS.replace("/* PRODUCTION_ALLOCATOR */", allocator)
    )
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         str(path / "test.c"), "-o", str(path / "test")], check=True
    )
    subprocess.run([str(path / "test")], check=True)
