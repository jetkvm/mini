#!/usr/bin/env python3
"""Exercise the real compact-ring policy and allocator ownership on the host."""

import ast
from pathlib import Path
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
source = (COMPONENT / "jetkvm_video.c").read_text()


def extract_function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


policy = extract_function(source, "static bool csi_use_compact_ring(")
allocator = extract_function(source, "static bool IRAM_ATTR csi_get_new_trans(")

# Reuse the existing ownership/lock-order harness, without executing that test.
tree = ast.parse((COMPONENT / "tests/test_csi_ready_reclaim.py").read_text())
harness = next(
    ast.literal_eval(node.value)
    for node in tree.body
    if isinstance(node, ast.Assign)
    and any(isinstance(target, ast.Name) and target.id == "HARNESS"
            for target in node.targets)
)
prefix = harness[:harness.index("static app_ctx_t saturated(void)")]
prefix = prefix.replace("bool use_driver_backup;",
                        "bool use_driver_backup, preview_only_active;")
prefix = prefix.replace("/* PRODUCTION_ALLOCATOR */", allocator)
prefix += "typedef struct { uint16_t width, height, fps; } tc358743_timings_t;\n"
prefix += policy

MAIN = r"""
static app_ctx_t saturated_four(void)
{
    static uint8_t buffers[6];
    app_ctx_t ctx = {0};
    ctx.capture_buffer_count = 4;
    ctx.capture_buffer_capacity = 3110400;
    ctx.inflight_buffers = 0x03; /* Two DMA descriptors, including this EOF. */
    ctx.locked_buffers = 0x0c;   /* Encoding 2, complete READY 3. */
    ctx.direct_h264_active = true;
    ctx.direct_h264_encoding_index = 2;
    ctx.direct_h264_ready_index = 3;
    ctx.direct_h264_generation = 7;
    ctx.direct_h264_slots[3].generation = 7;
    ctx.direct_h264_slots[3].capture_complete = true;
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

static void test_policy(void)
{
    struct {
        uint16_t width, height, fps;
        bool direct, preview, backup, compact;
    } cases[] = {
        {1920, 1080, 30, true,  false, false, true},
        {1920, 1080, 30, true,  true,  false, false},
        {1920, 1080, 30, true,  false, true,  false},
        {1920, 1080, 30, true,  true,  true,  false},
        {1920, 1080, 30, false, false, false, false},
        {1280, 720,  30, true,  false, false, false},
        {1280, 720,  60, true,  false, false, false},
        {1920, 1080, 0,  true,  false, false, false},
        {1920, 1080, 24, true,  false, false, false},
        {1920, 1080, 25, true,  false, false, false},
        {1920, 1080, 29, true,  false, false, false},
        {1920, 1080, 31, true,  false, false, false},
        {1920, 1080, 60, true,  false, false, false},
        {1919, 1080, 30, true,  false, false, false},
        {1920, 1082, 30, true,  false, false, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        app_ctx_t ctx = {0};
        ctx.preview_only_active = cases[i].preview;
        ctx.use_driver_backup = cases[i].backup;
        tc358743_timings_t timings = {
            cases[i].width, cases[i].height, cases[i].fps,
        };
        bool expected = cases[i].compact && !DIRECT_H264_OVERLAP_ENABLE;
        assert(csi_use_compact_ring(&ctx, &timings, cases[i].direct) == expected);
    }
}

static void test_four_buffer_ownership(void)
{
    app_ctx_t ctx = saturated_four();
    esp_cam_ctlr_trans_t trans = request(&ctx);
    assert(trans.buffer == ctx.capture_buffers[3]);
    assert(trans.buflen == ctx.capture_buffer_capacity);
    assert(ctx.inflight_buffers == 0x0b && ctx.locked_buffers == 0x04);
    assert(ctx.direct_h264_encoding_index == 2);
    assert(ctx.direct_h264_ready_index == -1);
    assert(ctx.metrics.capture_nofree_drops == 0);
    assert(ctx.metrics.direct_ready_replaced == 1);

    /* A real FREE buffer preserves the pending frame. */
    ctx = saturated_four();
    ctx.locked_buffers &= ~(1u << 2);
    ctx.free_buffers |= 1u << 2;
    ctx.direct_h264_encoding_index = -1;
    trans = request(&ctx);
    assert(trans.buffer == ctx.capture_buffers[2]);
    assert(ctx.direct_h264_ready_index == 3);
    assert(ctx.metrics.direct_ready_replaced == 0);

    /* Each DMA destination and the active encoder remain protected even if
     * stale READY metadata points to one of those buffers. */
    for (int i = 0; i < 3; ++i) {
        ctx = saturated_four();
        ctx.direct_h264_ready_index = i;
        ctx.direct_h264_slots[i] = ctx.direct_h264_slots[3];
        trans = request(&ctx);
        assert(trans.buffer == NULL);
        assert(ctx.inflight_buffers == 0x03 && ctx.locked_buffers == 0x0c);
        assert(ctx.metrics.capture_nofree_drops == 1);
    }

    /* Three descriptors plus four raw buffers cannot hand off a new EOF
     * while the previous frame still encodes and no READY is reclaimable. */
    ctx = saturated_four();
    ctx.inflight_buffers = 0x07;
    ctx.locked_buffers = 0x08;
    ctx.direct_h264_encoding_index = 3;
    ctx.direct_h264_ready_index = -1;
    trans = request(&ctx);
    assert(trans.buffer == NULL);
    assert(ctx.metrics.capture_nofree_drops == 1);
}

int main(void)
{
    test_policy();
    test_four_buffer_ownership();
    puts("CSI compact-mode selection and four-buffer ownership: PASS");
}
"""

with tempfile.TemporaryDirectory(prefix="csi-compact-ring-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(prefix + MAIN)
    for overlap in (0, 1):
        binary = path / f"test-overlap-{overlap}"
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
             f"-DDIRECT_H264_OVERLAP_ENABLE={overlap}", str(path / "test.c"),
             "-o", str(binary)], check=True,
        )
        subprocess.run([str(binary)], check=True)
