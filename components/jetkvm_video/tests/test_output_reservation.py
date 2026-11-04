#!/usr/bin/env python3
"""Exercise the production encoder handoff with consuming transport leases.

The transport stub frees its output on both commit and cancel. Sanitizers then
catch use after either ownership transfer, while assertions verify that frames
without an output slot never advance the encoder or consume a sync request.
"""

from pathlib import Path
import os
import re
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
source = (COMPONENT / "jetkvm_video.c").read_text()
header = (COMPONENT.parent / "webrtc_transport/include/webrtc_transport.h").read_text()


def function(signature):
    match = re.search(re.escape(signature) + r"[^;{]+\{", source)
    assert match, signature
    start = match.start()
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


def structure(name):
    return re.search(
        r"typedef struct \{(?:(?!typedef struct).)*?\} " + name + r";",
        header,
        re.S,
    )[0]


production = "\n".join(function(signature) for signature in (
    "static uint32_t h264_target_bitrate(",
    "static bool h264_hw_update_quality(",
    "static void h264_sync_idr_arm(",
    "static void h264_sync_idr_production_complete(",
    "static void video_metrics_add_encode_time(",
    "static void video_metrics_record_failed_encode(",
    "static void video_metrics_record_frame(",
    "static void h264_pipeline_encode_one(",
))
timing_window = re.search(
    r"typedef struct \{(?:(?!typedef struct).)*?\} video_timing_window_t;", source, re.S
)[0]
timing_samples = re.search(r"^#define VIDEO_TIMING_WINDOW_SAMPLES\s+[^\n]+", source, re.M)[0]
bitrate_defines = "\n".join(re.search(
    r"^#define " + name + r"\s+[^\n]+", source, re.M
)[0] for name in ("H264_1080P_TARGET_BITRATE_BPS", "H264_TARGET_BITRATE_MAX_BPS"))
# The totals since boot have one writer and one reader, and nothing sets
# them back: a new mode or a restart of capture leaves them as they are.
assert source.count("s_video_encode_totals") == 3
# Runtime preference application belongs exclusively to the encode worker.
assert source.count("h264_hw_update_quality(") == 2
assert "jetkvm_video_quality_" not in function("static void hdmi_capture_task(")
metric_fields = sorted(set(re.findall(r"(?:ctx->metrics\.|metrics->)(\w+)", production)) |
                       {"encode_output_skips"})
types = "\n".join(structure(name) for name in (
    "app_h264_frame_meta_t", "webrtc_transport_au_reservation_t",
))

PREAMBLE = r"""
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video_encode_totals.h"
#define DIRECT_H264_OVERLAP_EOF_WAIT_US 6000
#define ESP_H264_ERR_OK 0
#define JETKVM_VIDEO_QUALITY_AUTO_PERMILLE 0U
#define JETKVM_VIDEO_QUALITY_MAX_PERMILLE 1000U
#define ESP_H264_ERR_MEM (-3)
#define ESP_H264_ERR_OVERFLOW (-4)
#define ESP_H264_FRAME_TYPE_IDR 1
#define ESP_H264_FRAME_TYPE_P 2
#define TAG "host"
/* Keep log arguments type checked and considered used without printing them. */
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
typedef int esp_h264_err_t;
typedef struct { uint8_t *buffer; size_t len; } raw_data_t;
typedef struct { raw_data_t raw_data; int64_t pts; } esp_h264_enc_in_frame_t;
typedef struct {
    raw_data_t raw_data;
    uint32_t length;
    int frame_type;
} esp_h264_enc_out_frame_t;
typedef struct { int base; } fake_encoder_param_t;
typedef fake_encoder_param_t *esp_h264_enc_param_hw_handle_t;
typedef struct {
    uint32_t generation, capture_sequence;
    int64_t csi_dma_eof_timestamp_us;
    bool capture_complete, cancelled;
} direct_h264_slot_t;
"""

CONTEXT = r"""
typedef struct {
    void *h264_enc;
    uint8_t *h264_output_buf;
    size_t h264_output_len;
    uint16_t h264_width, h264_height, h264_fps, h264_gop;
    uint32_t h264_sync_idr_generation, h264_bitrate;
    direct_h264_slot_t direct_h264_slots[6];
    struct {
        /* METRICS */
    } metrics;
    video_timing_window_t timing_window;
} app_ctx_t;
typedef __typeof__(((app_ctx_t *)0)->metrics) video_metrics_t;
/* Since boot: reset() starts a new context, as a new mode does, and leaves
 * these. totals_at_reset is their copy at the last reset(). */
static video_encode_totals_t s_video_encode_totals, totals_at_reset;

static struct {
    bool available, commit_accepts, lease_live, force_next_idr, guest_owns;
    size_t capacity;
    uint8_t *lease_data;
    unsigned reserves, encodes, commits, cancels, take_sync, complete_sync;
    unsigned force_idr, delays;
    unsigned bitrate_updates, param_reads;
    uint32_t quality_permille, applied_bitrate, network_ceiling;
    bool adaptive;
    int param_result, bitrate_result;
    uint32_t pending_sync, last_completed_sync;
    bool last_completed_success;
    int encode_result;
    uint32_t encoded_length;
    int64_t now_us;
    app_h264_frame_meta_t committed_meta;
} state;
static fake_encoder_param_t encoder_param;
static uint8_t raw_input[64];
static int s_direct_h264_pipeline_mux, s_video_metrics_mux, lock_depth;
#define portENTER_CRITICAL(mux) do { (void)(mux); assert(!lock_depth); ++lock_depth; } while (0)
#define portEXIT_CRITICAL(mux) do { (void)(mux); assert(lock_depth == 1); --lock_depth; } while (0)
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(unsigned ticks)
{
    assert(!lock_depth && ticks == 1);
    ++state.delays;
}
static int64_t esp_timer_get_time(void)
{
    state.now_us += 1000;
    return state.now_us;
}
static bool webrtc_transport_reserve_access_unit(
    uint16_t width, uint16_t height, uint16_t fps,
    webrtc_transport_au_reservation_t *reservation)
{
    assert(!lock_depth && !state.lease_live);
    assert(width == 1920 && height == 1080 && fps == 30);
    ++state.reserves;
    if (!state.available) return false;
    state.lease_data = malloc(state.capacity);
    assert(state.lease_data);
    memset(state.lease_data, 0xa5, state.capacity);
    state.lease_live = true;
    *reservation = (webrtc_transport_au_reservation_t) {
        .data = state.lease_data, .capacity = state.capacity,
        .token = state.reserves, .generation = 7, .slot = 0,
    };
    return true;
}
static void consume_lease(webrtc_transport_au_reservation_t *reservation)
{
    assert(state.lease_live && reservation->data == state.lease_data);
    assert(reservation->token == state.reserves);
    free(reservation->data);
    state.lease_data = NULL;
    state.lease_live = false;
    *reservation = (webrtc_transport_au_reservation_t) {0};
}
static void webrtc_transport_cancel_access_unit(
    webrtc_transport_au_reservation_t *reservation)
{
    assert(!lock_depth);
    ++state.cancels;
    consume_lease(reservation);
}
static bool webrtc_transport_commit_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t length,
    const app_h264_frame_meta_t *meta)
{
    assert(!lock_depth && state.lease_live && state.encodes > 0);
    assert(reservation->data == state.lease_data);
    assert(length == state.encoded_length && length > 0);
    assert(length <= reservation->capacity);
    assert(reservation->data[0] == 0x65 || reservation->data[0] == 0x61);
    ++state.commits;
    state.committed_meta = *meta;
    consume_lease(reservation); /* Rejection also consumes the reservation. */
    return state.commit_accepts;
}
static bool jetkvm_video_guest_owns_display(void) { return state.guest_owns; }
static bool webrtc_transport_take_sync_idr_request(uint32_t *generation)
{
    ++state.take_sync;
    if (!state.pending_sync) return false;
    *generation = state.pending_sync;
    state.pending_sync = 0;
    return true;
}
static void webrtc_transport_complete_sync_idr_production(
    uint32_t generation, bool success)
{
    assert(generation);
    ++state.complete_sync;
    state.last_completed_sync = generation;
    state.last_completed_success = success;
    if (!success) state.pending_sync = generation;
}
static esp_h264_err_t esp_h264_enc_hw_get_param_hd(
    void *encoder, esp_h264_enc_param_hw_handle_t *param)
{
    assert(encoder == &encoder_param);
    ++state.param_reads;
    if (state.param_result != ESP_H264_ERR_OK) return state.param_result;
    *param = &encoder_param;
    return ESP_H264_ERR_OK;
}
static uint32_t webrtc_transport_video_bitrate(uint32_t ceiling, bool adaptive)
{
    state.adaptive = adaptive;
    if (!adaptive) return ceiling;
    return state.network_ceiling && state.network_ceiling < ceiling ? state.network_ceiling : ceiling;
}
static uint32_t jetkvm_video_quality_get_permille(void)
{
    return state.quality_permille;
}
static esp_h264_err_t esp_h264_enc_set_bitrate(int *base, uint32_t bitrate)
{
    /* Preference application must precede acquiring an output/DMA lease. */
    assert(base == &encoder_param.base && !lock_depth && !state.lease_live);
    ++state.bitrate_updates;
    if (state.bitrate_result != ESP_H264_ERR_OK) return state.bitrate_result;
    state.applied_bitrate = bitrate;
    return ESP_H264_ERR_OK;
}
static esp_h264_err_t esp_h264_enc_force_idr(int *base)
{
    assert(base == &encoder_param.base);
    ++state.force_idr;
    state.force_next_idr = true;
    return ESP_H264_ERR_OK;
}
static esp_h264_err_t esp_h264_enc_process(
    void *encoder, const esp_h264_enc_in_frame_t *in,
    esp_h264_enc_out_frame_t *out)
{
    assert(!lock_depth && encoder == &encoder_param);
    assert(state.lease_live && out->raw_data.buffer == state.lease_data);
    assert(out->raw_data.len == 512 && out->raw_data.len <= state.capacity);
    assert(in->raw_data.buffer == raw_input && in->raw_data.len == sizeof(raw_input));
    assert(in->pts == 90000);
    ++state.encodes;
    out->frame_type = state.force_next_idr ? ESP_H264_FRAME_TYPE_IDR : ESP_H264_FRAME_TYPE_P;
    memset(out->raw_data.buffer, 0, out->raw_data.len);
    out->raw_data.buffer[0] = state.force_next_idr ? 0x65 : 0x61;
    out->length = state.encoded_length;
    state.force_next_idr = false;
    return state.encode_result;
}
""".replace("/* METRICS */", "\n".join("uint64_t " + name + ";" for name in metric_fields))

CHECKS = r"""
static app_ctx_t reset(void)
{
    assert(!state.lease_live && !lock_depth);
    totals_at_reset = s_video_encode_totals;
    memset(&state, 0, sizeof(state));
    state.available = state.commit_accepts = true;
    state.capacity = 512;
    state.encoded_length = 16;
    state.now_us = 99000;
    state.quality_permille = 1000;
    state.applied_bitrate = 7000000;
    return (app_ctx_t) {
        .h264_enc = &encoder_param,
        .h264_output_buf = NULL, /* Ordinary YUV encoding has no fallback. */
        .h264_output_len = 512,
        .h264_width = 1920, .h264_height = 1080, .h264_fps = 30, .h264_gop = 15,
        .h264_bitrate = 7000000,
    };
}
static void encode(app_ctx_t *ctx, uint32_t capture_sequence)
{
    h264_pipeline_encode_one(ctx, raw_input, sizeof(raw_input),
                             capture_sequence, 90000, "host", -1, 0);
    assert(!state.lease_live && !lock_depth);
}
static uint32_t p_frames_since_reset(void)
{
    return s_video_encode_totals.p_frames - totals_at_reset.p_frames;
}
static uint32_t idr_frames_since_reset(void)
{
    return s_video_encode_totals.idr_frames - totals_at_reset.idr_frames;
}
static void assert_never_started(const app_ctx_t *ctx)
{
    assert(p_frames_since_reset() == 0 && idr_frames_since_reset() == 0);
    assert(state.encodes == 0 && state.take_sync == 0);
    assert(state.complete_sync == 0 && state.force_idr == 0);
    assert(state.commits == 0 && ctx->timing_window.handoff_count == 0);
    assert(ctx->metrics.encode_attempts == 0);
}
static void assert_recoverable_failure(app_ctx_t *ctx)
{
    /* A failed encode has no frame type: it is in no total. */
    assert(p_frames_since_reset() == 0 && idr_frames_since_reset() == 0);
    assert(ctx->metrics.encode_failures == 1 && ctx->metrics.encode_attempts == 1);
    assert(ctx->metrics.encode_frames == 0 && ctx->metrics.sink_frames == 0);
    assert(state.encodes == 1 && state.commits == 0 && state.cancels == 1);
    assert(state.force_next_idr && state.force_idr == 2);
    assert(state.pending_sync == 41 && ctx->h264_sync_idr_generation == 0);
    assert(state.complete_sync == 1 && !state.last_completed_success);
    assert(ctx->timing_window.handoff_count == 0);
    state.encode_result = ESP_H264_ERR_OK;
    state.encoded_length = 16;
    encode(ctx, 3);
    assert(state.committed_meta.keyframe);
    assert(state.committed_meta.sync_idr_generation == 41);
    assert(state.pending_sync == 0 && state.last_completed_success);
    assert(ctx->metrics.sink_frames == 1 && !state.force_next_idr);
    assert(p_frames_since_reset() == 0 && idr_frames_since_reset() == 1);
}
int main(void)
{
    /* Existing quality formula, including its floor, cap and integer rounding. */
    assert(h264_target_bitrate(1920, 1080, 30, 1000) == 7000000);
    assert(h264_target_bitrate(1920, 1080, 30, 500) == 3500000);
    assert(h264_target_bitrate(1280, 720, 60, 1000) == 7372800);
    assert(h264_target_bitrate(1280, 720, 59, 733) == 5314191);
    assert(h264_target_bitrate(640, 480, 30, 500) == 1000000);
    assert(h264_target_bitrate(3840, 2160, 60, 1000) == 12000000);

    /* Invalid inputs cannot reserve a slot or consume pending sync state. */
    app_ctx_t ctx = reset();
    ctx.h264_enc = NULL;
    encode(&ctx, 2);
    assert(ctx.metrics.encode_failures == 1 && state.reserves == 0);
    assert_never_started(&ctx);
    ctx = reset();
    h264_pipeline_encode_one(&ctx, NULL, sizeof(raw_input), 2, 90000, "host", -1, 0);
    assert(ctx.metrics.encode_failures == 1 && state.reserves == 0);
    assert_never_started(&ctx);
    ctx = reset();
    h264_pipeline_encode_one(&ctx, raw_input, 0, 2, 90000, "host", -1, 0);
    assert(ctx.metrics.encode_failures == 1 && state.reserves == 0);
    assert_never_started(&ctx);

    /* Network feedback changes RC in place without resetting references.
     * Only Auto asks the transport for an adaptive target; its ceiling is
     * the High rate. A manual preset is a fixed rate. */
    ctx = reset();
    state.network_ceiling = 1800000;
    state.quality_permille = JETKVM_VIDEO_QUALITY_AUTO_PERMILLE;
    assert(h264_hw_update_quality(&ctx));
    assert(state.adaptive);
    assert(ctx.h264_bitrate == 1800000 && state.applied_bitrate == 1800000);
    assert(state.force_idr == 0 && state.encodes == 0 && state.reserves == 0);
    state.network_ceiling = 0;
    assert(h264_hw_update_quality(&ctx));
    assert(ctx.h264_bitrate == 7000000 && state.applied_bitrate == 7000000);
    state.network_ceiling = 1800000;
    state.quality_permille = 1000;
    assert(h264_hw_update_quality(&ctx));
    assert(!state.adaptive);
    assert(ctx.h264_bitrate == 7000000 && state.applied_bitrate == 7000000);

    /* Changing quality updates the existing encoder before its next frame.
     * Repeated/clamped values need no SDK write, reference reset or new IDR. */
    ctx = reset();
    encode(&ctx, 1);
    assert(state.bitrate_updates == 0 && state.param_reads == 0);
    state.quality_permille = 500;
    encode(&ctx, 2);
    assert(ctx.h264_bitrate == 3500000 && state.applied_bitrate == 3500000);
    assert(state.bitrate_updates == 1 && state.force_idr == 0);
    assert(ctx.h264_enc == &encoder_param && ctx.h264_gop == 15);
    assert(!state.committed_meta.keyframe && state.commits == 2);
    encode(&ctx, 3);
    assert(state.bitrate_updates == 1 && state.commits == 3);
    state.quality_permille = 1000;
    encode(&ctx, 4);
    assert(ctx.h264_bitrate == 7000000 && state.applied_bitrate == 7000000);
    assert(state.bitrate_updates == 2 && state.force_idr == 0);
    state.quality_permille = 100;
    encode(&ctx, 5);
    assert(ctx.h264_bitrate == 1000000 && state.bitrate_updates == 3);
    state.quality_permille = 101;
    encode(&ctx, 6);
    assert(ctx.h264_bitrate == 1000000 && state.bitrate_updates == 3);

    /* A failed lookup/set does not reserve an AU, encode with an unconfirmed
     * preference, consume sync state, or mutate the cached applied bitrate.
     * The same requested value is retried and applied on the following job. */
    for (unsigned lookup_failure = 0; lookup_failure < 2; ++lookup_failure) {
        ctx = reset();
        state.quality_permille = 500;
        state.pending_sync = 41;
        if (lookup_failure) state.param_result = -99;
        else state.bitrate_result = -99;
        encode(&ctx, 2);
        assert_never_started(&ctx);
        assert(state.reserves == 0 && state.cancels == 0);
        assert(ctx.metrics.encode_failures == 1 && ctx.metrics.encode_attempts == 0);
        assert(ctx.h264_bitrate == 7000000 && state.applied_bitrate == 7000000);
        assert(state.pending_sync == 41 && state.bitrate_updates == !lookup_failure);
        state.param_result = state.bitrate_result = ESP_H264_ERR_OK;
        encode(&ctx, 3);
        assert(ctx.h264_bitrate == 3500000 && state.applied_bitrate == 3500000);
        assert(state.commits == 1 && state.committed_meta.sync_idr_generation == 41);
        assert(state.force_idr == 1 && state.last_completed_success);
    }

    /* A full or inactive transport drops raw work before arming the encoder. */
    ctx = reset();
    state.available = false;
    state.pending_sync = 41;
    encode(&ctx, 2);
    assert_never_started(&ctx);
    assert(state.pending_sync == 41 && state.reserves == 1 && state.cancels == 0);
    assert(ctx.metrics.encode_output_skips == 1 && ctx.metrics.encode_failures == 0);
    assert(ctx.metrics.encode_attempts == 0 && ctx.metrics.sink_drops == 0);
    ctx.h264_sync_idr_generation = 7;
    encode(&ctx, 3);
    assert_never_started(&ctx);
    assert(ctx.h264_sync_idr_generation == 7 && state.pending_sync == 41);
    assert(ctx.metrics.encode_output_skips == 2);

    /* OS Services guest video owns the transport: no reservation, and the
     * sync IDR request stays for the OS Services task. */
    ctx = reset();
    state.guest_owns = true;
    state.pending_sync = 41;
    encode(&ctx, 2);
    assert_never_started(&ctx);
    assert(state.reserves == 0 && state.take_sync == 0 && state.pending_sync == 41);
    assert(ctx.metrics.encode_output_skips == 1 && ctx.metrics.encode_failures == 0);
    state.guest_owns = false;

    /* Configuration mismatch relinquishes the undersized lease before DMA. */
    ctx = reset();
    state.capacity = 256;
    state.pending_sync = 41;
    encode(&ctx, 2);
    assert_never_started(&ctx);
    assert(state.reserves == 1 && state.cancels == 1 && state.pending_sync == 41);
    assert(ctx.metrics.encode_failures == 1 && ctx.metrics.encode_output_skips == 0);

    /* A successful IDR and P frame both use the lease throughout encoding. */
    ctx = reset();
    state.pending_sync = 41;
    encode(&ctx, 2);
    assert(state.encodes == 1 && state.commits == 1 && state.cancels == 0);
    assert(ctx.metrics.encode_frames == 1 && ctx.metrics.sink_frames == 1);
    assert(ctx.metrics.encoded_bytes == 16 && ctx.metrics.encode_keyframes == 1);
    assert(ctx.metrics.encode_time_us == 1000 && ctx.metrics.encode_time_max_us == 1000);
    /* One timing sample per frame, in the same accounting as its counters:
     * the handoff runs from the 90000 us capture to the encode start. */
    assert(ctx.timing_window.handoff_count == 1);
    assert(ctx.timing_window.handoff_us[0] ==
           (uint32_t)(state.committed_meta.encode_start_timestamp_us - 90000));
    assert(ctx.timing_window.idr_encode_count == 1 && ctx.timing_window.idr_encode_us[0] == 1000);
    assert(ctx.timing_window.p_encode_count == 0);
    /* The totals since boot take the same frame: its type, its encode time
     * and its handoff. */
    assert(idr_frames_since_reset() == 1 && p_frames_since_reset() == 0);
    assert(s_video_encode_totals.idr_encode_us - totals_at_reset.idr_encode_us == 1000);
    assert(s_video_encode_totals.p_encode_us == totals_at_reset.p_encode_us);
    assert(s_video_encode_totals.handoff_us - totals_at_reset.handoff_us == 10000);
    assert(state.complete_sync == 1 && state.last_completed_sync == 41);
    assert(state.last_completed_success && ctx.h264_sync_idr_generation == 0);
    assert(state.committed_meta.sequence == 1 && state.committed_meta.capture_sequence == 2);
    assert(state.committed_meta.capture_timestamp_us == 90000);
    assert(state.committed_meta.capture_eof_timestamp_us == 90000);
    assert(state.committed_meta.encode_start_timestamp_us == 100000);
    assert(state.committed_meta.encode_done_timestamp_us == 101000);
    assert(state.committed_meta.rtp_timestamp_90khz == 8100);
    assert(state.committed_meta.width == 1920 && state.committed_meta.height == 1080);
    assert(state.committed_meta.fps == 30 && state.committed_meta.keyframe);
    assert(!state.committed_meta.encode_overlapped_capture);
    encode(&ctx, 4);
    assert(state.commits == 2 && state.cancels == 0 && state.force_idr == 1);
    assert(!state.committed_meta.keyframe && state.committed_meta.sync_idr_generation == 0);
    assert(ctx.metrics.capture_sequence_gaps == 1 && ctx.metrics.encode_frames == 2);
    assert(idr_frames_since_reset() == 1 && p_frames_since_reset() == 1);
    assert(s_video_encode_totals.p_encode_us - totals_at_reset.p_encode_us ==
           (uint64_t)(state.committed_meta.encode_done_timestamp_us -
                      state.committed_meta.encode_start_timestamp_us));

    /* Encoder errors and empty output retire/requeue sync and restart the
     * reference chain. Failed output never reaches the consuming commit. */
    const int failures[] = { ESP_H264_ERR_MEM, ESP_H264_ERR_OVERFLOW, -99, ESP_H264_ERR_OK };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        ctx = reset();
        state.pending_sync = 41;
        state.encode_result = failures[i];
        if (state.encode_result == ESP_H264_ERR_OK) state.encoded_length = 0;
        encode(&ctx, 2);
        assert(ctx.metrics.encode_overflows == (i < 2));
        assert_recoverable_failure(&ctx);
    }

    /* Overlap is disabled in production modes, but unsafe completion must
     * never publish an already encoded reference if a diagnostic enables it. */
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        ctx = reset();
        state.pending_sync = 41;
        ctx.direct_h264_slots[0] = (direct_h264_slot_t) {
            .generation = scenario == 2 ? 8 : 7,
            .capture_sequence = 2,
            .capture_complete = scenario != 1,
            .csi_dma_eof_timestamp_us = 101001, /* Encoder completed at 101000. */
        };
        h264_pipeline_encode_one(&ctx, raw_input, sizeof(raw_input),
                                 2, 90000, "host", 0, 7);
        assert(!state.lease_live && !lock_depth);
        assert(ctx.direct_h264_slots[0].cancelled == (scenario != 2));
        assert_recoverable_failure(&ctx);
    }

    /* A rejected commit has already consumed its lease. It must not trigger
     * a second cancel or let a later P frame depend on the rejected picture. */
    for (unsigned pending = 0; pending < 2; ++pending) {
        ctx = reset();
        state.pending_sync = pending ? 41 : 0;
        state.commit_accepts = false;
        encode(&ctx, 2);
        assert(state.encodes == 1 && state.commits == 1 && state.cancels == 0);
        assert(ctx.metrics.encode_frames == 1 && ctx.metrics.sink_drops == 1);
        /* The encoder made the access unit: its time counts, sent or not. */
        assert(p_frames_since_reset() + idr_frames_since_reset() == 1);
        assert(ctx.metrics.sink_frames == 0 && ctx.metrics.encode_failures == 0);
        assert(state.force_next_idr && state.force_idr == 1 + pending);
        assert(state.pending_sync == (pending ? 41u : 0u));
        assert(ctx.h264_sync_idr_generation == 0);
        state.commit_accepts = true;
        encode(&ctx, 3);
        assert(state.committed_meta.keyframe && ctx.metrics.sink_frames == 1);
        assert(state.commits == 2 && state.cancels == 0 && !state.force_next_idr);
        if (pending) assert(state.last_completed_success && state.pending_sync == 0);
    }
    puts("Encoder output leases: bitrate retune/retry, skip before IDR, DMA ownership, cancellation, reference recovery: PASS");
}
"""

with tempfile.TemporaryDirectory(prefix="video-output-reservation-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(PREAMBLE + bitrate_defines + "\n" + timing_samples + "\n" +
                                     timing_window + "\n" + types + CONTEXT + production + CHECKS)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-I", str(COMPONENT),
        str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    # LeakSanitizer does not work when the process runs under ptrace
    # (debuggers, ptrace-based sandboxes), so leak detection is off.
    # Lease balance is asserted explicitly; address/undefined checks stay on.
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    subprocess.run([str(path / "test")], check=True, env=environment)
