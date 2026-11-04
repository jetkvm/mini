/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "sdkconfig.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HDMI_HARDWARE_DMA_ONLY 1

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/isp.h"
#include "driver/i2c_master.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "p4_csi_capture.h"
#include "dma_cache_fastpath.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_idf_version.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/esp_cache_private.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_rom_sys.h"
#include "hal/color_types.h"
#include "hal/mipi_csi_phy_ll.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "soc/isp_struct.h"
#include "soc/reg_base.h"
#include "soc/mipi_csi_bridge_struct.h"
#include "soc/mipi_csi_host_struct.h"
#include "soc/dw_gdma_struct.h"

#include "device_api.h"
#include "device_contract.h"
#include "jetkvm_board_config.h"
#include "jetkvm_hdmi_bridge.h"
#include "tc358743_regs.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_screen.h"
#include "jetkvm_settings.h"
#include "jetkvm_video.h"
#include "jetkvm_video_helpers.h"
#include "jetkvm_video_quality.h"
#include "video_encode_totals.h"
#include "hdmi_hpd_retry.h"
#include "hdmi_health.h"
#include "webrtc_transport.h"

#define TC_CSI_YUV422_DATA_TYPE    0x1e
#define TC_CSI_USE_RAW_DATA_TYPE   0
#define TC_CSI_INPUT_YUV422_COLOR  CAM_CTLR_COLOR_YUV422_YVYU
#define TC_CSI_OUTPUT_YUV_COLOR    CAM_CTLR_COLOR_YUV420
/* Rev3's full four-byte reverse of each IDI32 word restores the adjacent-pixel
 * order; the matching CSI_CM input declaration must therefore be reversed to
 * YVYU as well. */
#define TC_CSI_INPUT_8BIT_SWAP     1
#define TC_CSI_INPUT_16BIT_SWAP    0
#define TC_CSI_OUTPUT_BYTE_SWAP    0
#define TC_H264_RAW_FORMAT         ESP_H264_RAW_FMT_O_UYY_E_VYY

#define FRAME_WIDTH_MAX             1920
#define FRAME_HEIGHT_MAX            1080
#define FRAME_BUFFER_MAX_BYTES      (FRAME_WIDTH_MAX * FRAME_HEIGHT_MAX * 3)
#define CSI_FRAME_BUFFER_COUNT      6
#define CSI_FRAME_BUFFER_COUNT_DEFAULT 5
#define CSI_FRAME_BUFFER_COUNT_DIRECT_1080P30 5
#define CSI_FRAME_BUFFER_COUNT_COMPACT_1080P30 4
#define CSI_FRAME_BUFFER_COUNT_DIRECT_HIGH_RATE 6
/* Fixed video pool: six capture slots of the largest direct-capture
 * frame and one H.264 DB buffer for 1920x1080, allocated once at boot. A mode
 * change uses the first slots it needs and never frees or requests them. */
#define VIDEO_POOL_CAPTURE_SLOTS    CSI_FRAME_BUFFER_COUNT_DIRECT_HIGH_RATE
#define VIDEO_POOL_CAPTURE_SLOT_BYTES (FRAME_WIDTH_MAX * FRAME_HEIGHT_MAX * 3 / 2)
#define CSI_BUFFER_INVALID_INDEX    0xff
#define DIRECT_H264_INVALID_INDEX  (-1)
#define DIRECT_H264_DRAIN_MS       3000
#define DIRECT_H264_TASK_STACK      (4 * 1024)
#define SCREEN_PREVIEW_TASK_STACK   (3 * 1024)
#define SCREEN_PREVIEW_INTERVAL_US  0
#define SCREEN_PREVIEW_DRAIN_MS     3000
#define VIDEO_MODE_BUTTON_GPIO      ((gpio_num_t)JETKVM_VIDEO_MODE_BUTTON_PIN)
#define VIDEO_MODE_BUTTON_TASK_STACK (2 * 1024)
#define VIDEO_MODE_BUTTON_POLL_MS   10
#define VIDEO_MODE_BUTTON_DEBOUNCE_SAMPLES 3
/* With network available, keep the generated KVM UI on the local display
 * while capture feeds H.264/WebRTC. Link-down fallback and SW3 can select the
 * exclusive HDMI preview path. */
#define VIDEO_MODE_DEFAULT_PREVIEW_ONLY 0
/* Timing for the optional overlap path, in which the encoder starts on a CSI
 * frame before its end-of-frame interrupt. DIRECT_H264_OVERLAP_ENABLE below
 * turns that path on and is 0 (off): end-of-encode timing alone cannot prove
 * that every row was captured before it was read. */
#define DIRECT_H264_OVERLAP_LEAD_US 3000
#define DIRECT_H264_OVERLAP_MAX_DISPATCH_US 6000
#define DIRECT_H264_OVERLAP_EOF_WAIT_US 6000
/* Encoding must only consume completed CSI frames. A fixed producer lead
 * cannot prevent the encoder from overtaking DMA, particularly on IDRs.
 * Rejecting an already encoded frame also leaves later P frames referring
 * to a picture that was never sent. Use the EOF handoff and buffer lock. */
#define DIRECT_H264_OVERLAP_ENABLE  0
#define VIDEO_MEDIA_CORE           1
#define FRAME_WAIT_MS               500
#define HDMI_CAPTURE_TASK_STACK     (12 * 1024)
#define H264_OUTPUT_MIN_BYTES       (512 * 1024)
/* Build-time diagnostic switches for HDMI capture and H.264 encoding. The
 * values below are the production settings: the dump, inspection, and
 * override switches are 0 (off), and the others select the production
 * capture path. */
/* Name of the capture candidate in s_candidates that the production path
 * uses. When this candidate cannot carry the input mode, or when the string
 * is empty, tc_try_candidates_held() uses its normal candidate order. */
#define HDMI_DEBUG_FORCE_CANDIDATE  "yuv594_f20"
#define HDMI_DEBUG_YUV_ONLY         1
#define HDMI_DEBUG_USE_DRIVER_BACKUP 0
/* p4_csi_capture refuses to expose its backup buffer while the CSI interrupt
 * is cache-safe (see s_csi_ctlr_get_internal_buffer). */
#if HDMI_DEBUG_USE_DRIVER_BACKUP && defined(CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE)
#error "HDMI_DEBUG_USE_DRIVER_BACKUP needs CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE off"
#endif
#define HDMI_DEBUG_CAPTURE_ACTIVE_DE 1
#define HDMI_DEBUG_FORCE_NO_HSYNC 0
#define HDMI_DEBUG_ASSUME_1080P30    0
#define H264_DEBUG_ENCODE_INTERVAL_FRAMES 1
#define H264_DEBUG_SAMPLE_INTERVAL_FRAMES 0
#define H264_DEBUG_PIXEL_INSPECTION 0
#define H264_SERIAL_DUMP_ENABLE 0
#define H264_SERIAL_DUMP_AFTER_FRAME 1
#define H264_SERIAL_DUMP_MIN_BYTES  3000
#define H264_SERIAL_DUMP_MAX_BYTES (256 * 1024)
#define RAW_SERIAL_DUMP_AFTER_FRAME 0
#define RAW_SERIAL_DUMP_MAX_BYTES  (64 * 1024)
#define H264_DEBUG_SYNTH_WIDTH      640
#define H264_DEBUG_SYNTH_HEIGHT     368
#define RGB_PREVIEW_DUMP_DECIMATE   4
#define CSI_STREAM_SETTLE_MS        100
#define CSI_REARM_AFTER_STREAM_ENABLE 0
#define CSI_REARM_CAPTURE_RETRIES   8
#define CSI_BUF_AFULL_THRD_OVERRIDE 224
#define CSI_DMA_BURST_LEN_OVERRIDE  64
#define CSI_DMA_BLOCK_BURSTS_MAX    0x1fff
/* One bridge DMA burst: dma_burst_len 64-bit words. */
#define CSI_DMA_BURST_BYTES         ((size_t)CSI_DMA_BURST_LEN_OVERRIDE * 8U)
/* Capture buffers hold whole bursts, so a block bound that fits the buffer
 * also fits every frame the buffer is allocated for. */
#define CSI_CAPTURE_CAPACITY(bytes) \
    (((size_t)(bytes) + CSI_DMA_BURST_BYTES - 1U) / CSI_DMA_BURST_BYTES * \
     CSI_DMA_BURST_BYTES)
#define CSI_FRAME_SYNC_TIMEOUT_US   50000
#define ACTIVE_TIMING_CHECK_MS      500
#define HDMI_CONNECTION_CHECK_MS    500
#define VIDEO_STATS_INTERVAL_US     (2 * 1000 * 1000)
#define VIDEO_HEAP_INTERVAL_US      (30 * 1000 * 1000)
#define VIDEO_TIMING_WINDOW_SAMPLES 96
#define H264_1080P_TARGET_BITRATE_BPS 7000000U
#define H264_TARGET_BITRATE_MAX_BPS   12000000U
#define H264_QUALITY_QP_MIN           28U
#define VIDEO_SLEEP_UPSTREAM_DEFAULT_SECONDS 60
/* Rebuild admission waits up to 1 s for a peer open already in flight. */
#define VIDEO_ADMISSION_WAIT_MS 20
#define VIDEO_ADMISSION_WAITS 50

static const char *TAG = "hdmi_csi";
static const uint32_t TC358743_REFCLK_HZ = 27000000;
static volatile bool s_video_hdmi_connected;
static volatile bool s_network_available = JETKVM_BOARD_WIFI != 0;
static volatile bool s_host_display_idle_enabled;
/* Session gap tolerated before hiding the host display (see below). */
#define HOST_DISPLAY_IDLE_GRACE_US (INT64_C(1800) * 1000)
static volatile int32_t s_video_sleep_after_seconds =
    JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS;
static volatile bool s_video_sleep_active;
static volatile bool s_capture_suspended;

_Static_assert(CSI_FRAME_BUFFER_COUNT == VIDEO_POOL_CAPTURE_SLOTS &&
               CSI_FRAME_BUFFER_COUNT_DEFAULT <= VIDEO_POOL_CAPTURE_SLOTS &&
               CSI_FRAME_BUFFER_COUNT_DIRECT_1080P30 <= VIDEO_POOL_CAPTURE_SLOTS &&
               CSI_FRAME_BUFFER_COUNT_COMPACT_1080P30 <= VIDEO_POOL_CAPTURE_SLOTS,
               "every capture mode must fit the video pool");
_Static_assert(TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420,
               "the pool slot holds one direct-capture YUV420 frame");

/* Allocated by jetkvm_video_reserve_buffers() and never freed. */
static struct {
    void *capture[VIDEO_POOL_CAPTURE_SLOTS];
    void *h264_db;
    uint32_t h264_db_bytes;
} s_video_pool;

/* What the capture task read from the receiver last, for "hdmiReceiver" in
 * /webrtc/stats. Under s_hdmi_receiver_mux. */
static portMUX_TYPE s_hdmi_receiver_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool status_read;
    uint8_t sys_status;
    int64_t status_us;
    bool detect_ran;
    esp_err_t detect_result;
    tc358743_timings_t detected;
    int64_t detect_us;
} s_hdmi_receiver;

static void video_update_hdmi_connection(jetkvm_hdmi_bridge_t *bridge,
                                        int64_t *next_check_us)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us < *next_check_us) {
        return;
    }
    uint8_t sys_status;
    if (tc_get_sys_status(bridge, &sys_status) == ESP_OK) {
        __atomic_store_n(&s_video_hdmi_connected,
                         (sys_status & MASK_S_DDC5V) != 0, __ATOMIC_RELEASE);
        portENTER_CRITICAL(&s_hdmi_receiver_mux);
        s_hdmi_receiver.status_read = true;
        s_hdmi_receiver.sys_status = sys_status;
        s_hdmi_receiver.status_us = now_us;
        portEXIT_CRITICAL(&s_hdmi_receiver_mux);
    }
    *next_check_us = now_us + HDMI_CONNECTION_CHECK_MS * INT64_C(1000);
}

/* tc_get_detected_timings() for the capture task, which keeps the result for
 * "hdmiReceiver". */
static esp_err_t video_detect_timings(jetkvm_hdmi_bridge_t *bridge,
                                      tc358743_timings_t *timings)
{
    const esp_err_t result = tc_get_detected_timings(bridge, timings);
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_hdmi_receiver_mux);
    s_hdmi_receiver.detect_ran = true;
    s_hdmi_receiver.detect_result = result;
    s_hdmi_receiver.detected = *timings;
    s_hdmi_receiver.detect_us = now_us;
    portEXIT_CRITICAL(&s_hdmi_receiver_mux);
    return result;
}

void jetkvm_video_get_hdmi_receiver_stats(
    webrtc_transport_hdmi_receiver_t *receiver)
{
    portENTER_CRITICAL(&s_hdmi_receiver_mux);
    const __typeof__(s_hdmi_receiver) last = s_hdmi_receiver;
    portEXIT_CRITICAL(&s_hdmi_receiver_mux);
    const int64_t now_us = esp_timer_get_time();
    *receiver = (webrtc_transport_hdmi_receiver_t) {
        .status_read = last.status_read,
        .sys_status = last.sys_status,
        .cable_5v = (last.sys_status & MASK_S_DDC5V) != 0,
        .tmds_clock = (last.sys_status & MASK_S_TMDS) != 0,
        .phy_pll = (last.sys_status & MASK_S_PHY_PLL) != 0,
        .phy_scdt = (last.sys_status & MASK_S_PHY_SCDT) != 0,
        .sync = (last.sys_status & MASK_S_SYNC) != 0,
        .status_age_ms = (uint64_t)(now_us - last.status_us) / 1000,
        .detect_ran = last.detect_ran,
        .detect_result = esp_err_to_name(last.detect_result),
        .detected_width = last.detected.width,
        .detected_height = last.detected.height,
        .detected_fps = last.detected.fps,
        .detect_age_ms = (uint64_t)(now_us - last.detect_us) / 1000,
    };
}

typedef struct {
    uint32_t generation;
    uint32_t capture_sequence;
    int64_t csi_dma_start_timestamp_us;
    int64_t csi_dma_eof_timestamp_us;
    bool capture_complete;
    bool overlap_candidate;
    bool cancelled;
} direct_h264_slot_t;

typedef struct {
    int8_t capture_buffer_index;
    uint32_t capture_sequence;
    size_t raw_size;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
} screen_preview_job_t;

typedef struct {
    uint32_t handoff_us[VIDEO_TIMING_WINDOW_SAMPLES];
    uint32_t p_encode_us[VIDEO_TIMING_WINDOW_SAMPLES];
    uint32_t idr_encode_us[VIDEO_TIMING_WINDOW_SAMPLES];
    uint16_t handoff_count;
    uint16_t p_encode_count;
    uint16_t idr_encode_count;
    uint32_t dropped_samples;
} video_timing_window_t;

typedef struct {
    uint16_t mode_width;
    uint16_t mode_height;
    uint16_t mode_fps;
    volatile uint32_t capture_frames;
    volatile uint32_t capture_nofree_drops;
    uint32_t capture_stale_drops;
    uint32_t capture_sequence_gaps;
    uint32_t last_encoded_capture_sequence;
    uint32_t encode_attempts;
    uint32_t encode_frames;
    uint32_t encode_failures;
    uint32_t encode_output_skips;
    uint32_t encode_overflows;
    uint32_t encode_keyframes;
    uint32_t sink_frames;
    uint32_t sink_drops;
    uint64_t encoded_bytes;
    uint64_t encode_time_us;
    uint32_t encode_time_max_us;
    uint32_t direct_ready_replaced;
    uint32_t direct_expired_drops;
    int64_t window_start_us;
    int64_t last_heap_log_us;
    uint32_t window_capture_frames;
    uint32_t window_capture_nofree_drops;
    uint32_t window_capture_stale_drops;
    uint32_t window_capture_sequence_gaps;
    uint32_t window_encode_attempts;
    uint32_t window_encode_frames;
    uint32_t window_encode_failures;
    uint32_t window_encode_output_skips;
    uint32_t window_encode_overflows;
    uint32_t window_sink_frames;
    uint32_t window_sink_drops;
    uint64_t window_encoded_bytes;
    uint64_t window_encode_time_us;
    uint32_t window_direct_ready_replaced;
    uint32_t window_direct_expired_drops;
    uint32_t window_http_drops;
} video_metrics_t;

typedef struct {
    /* TC358743 bridge driver state (i2c_bus, isp, refclk_hz,
     * active_candidate, log_tag) is composed into this context; driver entry
     * points receive &bridge and never see the capture state machine. */
    jetkvm_hdmi_bridge_t bridge;
    esp_ldo_channel_handle_t mipi_ldo;
    esp_cam_ctlr_handle_t cam;
    void *frame_buffer;
    size_t frame_buffer_size;
    size_t capture_buffer_capacity;
    /* Capacity of every capture_buffers[] slot. The slots are the video pool
     * (s_video_pool): a mode uses the first ones it needs, none is freed. */
    size_t capture_buffers_capacity;
    void *capture_buffers[CSI_FRAME_BUFFER_COUNT];
    dma_cache_fastpath_handle_t capture_cache_fastpaths[CSI_FRAME_BUFFER_COUNT];
    volatile size_t capture_received_sizes[CSI_FRAME_BUFFER_COUNT];
    volatile uint32_t capture_buffer_sequences[CSI_FRAME_BUFFER_COUNT];
    volatile int64_t capture_buffer_timestamps_us[CSI_FRAME_BUFFER_COUNT];
    uint8_t capture_buffer_count;
    volatile uint8_t capture_next_buffer;
    volatile uint32_t free_buffers;
    volatile uint32_t inflight_buffers;
    volatile uint32_t ready_buffers;
    volatile uint32_t locked_buffers;  /* bit N = buffer N is owned by encode/preview */
    volatile uint32_t csi_no_free_buffers;
    volatile uint32_t capture_sequence;
    uint32_t current_capture_sequence;
    int64_t current_capture_timestamp_us;
    bool frame_buffer_owned_by_driver;
    bool use_driver_backup;
    tc358743_timings_t active_timings;
    const tc358743_csi_preset_t *active_preset;
    TaskHandle_t capture_task;
    uint32_t frame_counter;
    uint32_t last_signature;
    volatile uint32_t frames_finished;
    uint32_t frames_consumed;
    uint32_t consecutive_timeouts;
    volatile size_t last_received_size;
    uint16_t capture_width;
    uint16_t capture_height;
    bool h264_serial_dumped;
    bool raw_serial_dumped;
    esp_h264_enc_handle_t h264_enc;
    uint8_t *h264_output_buf;
    uint32_t h264_output_len;
    uint16_t h264_width;
    uint16_t h264_height;
    uint8_t h264_fps;
    uint8_t h264_gop;
    uint32_t h264_bitrate;
    /* The transport refused the encoder's mode; cleared by csi_destroy(). */
    bool h264_mode_refused;
    esp_h264_raw_format_t h264_pic_type;
    uint32_t h264_sync_idr_generation;
    direct_h264_slot_t direct_h264_slots[CSI_FRAME_BUFFER_COUNT];
    volatile int8_t direct_h264_ready_index;
    volatile int8_t direct_h264_encoding_index;
    volatile bool direct_h264_active;
    volatile bool direct_h264_overlap_enabled;
    volatile uint32_t direct_h264_generation;
    bool video_sleep_resume_direct_pipeline;
    TaskHandle_t direct_h264_encode_task;
    TaskHandle_t screen_preview_task;
    QueueHandle_t screen_preview_queue;
    volatile uint32_t screen_preview_owned_jobs;
    int64_t screen_preview_last_enqueue_us;
    uint32_t screen_preview_last_logged_applied;
    uint32_t screen_preview_last_logged_ppa_frames;
    uint32_t screen_preview_last_logged_ppa_total_us;
    volatile bool preview_only_requested;
    bool preview_only_active;
    uint32_t h264_stream_dropped;
    bool screen_preview_logged;
    video_metrics_t metrics;
    video_timing_window_t timing_window;
} app_ctx_t;

static const tc358743_csi_preset_t s_csi_presets[] = {
    {
        .bps_per_lane = 357000000,
        .lineinitcnt = 0x0e80,
        .lptxtimecnt = 0x0003,
        .tclk_headercnt = 0x1403,
        .tclk_trailcnt = 0x0000,
        .ths_headercnt = 0x0103,
        .twakeup = 0x4882,
        .tclk_postcnt = 0x0008,
        .ths_trailcnt = 0x0002,
        .hstxvregcnt = 0x0000,
    },
    {
        .bps_per_lane = 594000000,
        .lineinitcnt = 0x0e80,
        .lptxtimecnt = 0x0003,
        .tclk_headercnt = 0x1403,
        .tclk_trailcnt = 0x0000,
        .ths_headercnt = 0x0103,
        .twakeup = 0x4882,
        .tclk_postcnt = 0x0008,
        .ths_trailcnt = 0x0002,
        .hstxvregcnt = 0x0000,
    },
    {
        .bps_per_lane = 972000000,
        .lineinitcnt = 0x1b58,
        .lptxtimecnt = 0x0007,
        .tclk_headercnt = 0x2806,
        .tclk_trailcnt = 0x0000,
        .ths_headercnt = 0x0806,
        .twakeup = 0x4268,
        .tclk_postcnt = 0x0008,
        .ths_trailcnt = 0x0005,
        .hstxvregcnt = 0x0000,
    },
    {
        .bps_per_lane = 1250000000,
        .lineinitcnt = 0x2200,
        .lptxtimecnt = 0x0008,
        .tclk_headercnt = 0x3208,
        .tclk_trailcnt = 0x0000,
        .ths_headercnt = 0x0a08,
        .twakeup = 0x4268,
        .tclk_postcnt = 0x0008,
        .ths_trailcnt = 0x0006,
        .hstxvregcnt = 0x0000,
    },
};

static esp_err_t tc_start_streaming(app_ctx_t *ctx, const tc358743_timings_t *timings, const tc358743_capture_candidate_t *candidate);
static esp_err_t tc_capture_one(app_ctx_t *ctx);
static esp_err_t isp_init_csi_passthrough(app_ctx_t *ctx);
static esp_err_t csi_restart_rx(app_ctx_t *ctx);
static esp_err_t csi_sync_rx_to_next_frame(app_ctx_t *ctx);
static esp_err_t p4_video_sleep_resume_capture(app_ctx_t *ctx);
static void h264_hw_deinit(app_ctx_t *ctx);
static bool h264_hw_matches(const app_ctx_t *ctx, uint16_t width,
                            uint16_t height, uint16_t fps,
                            esp_h264_raw_format_t pic_type);
static esp_err_t h264_hw_ensure(app_ctx_t *ctx, uint16_t width, uint16_t height,
                                uint16_t fps, esp_h264_raw_format_t pic_type);
static void h264_sync_idr_arm(app_ctx_t *ctx);
static void h264_sync_idr_production_complete(app_ctx_t *ctx, bool success);
static void video_metrics_begin_mode(app_ctx_t *ctx, uint16_t width, uint16_t height, uint16_t fps);
static void video_metrics_maybe_log(app_ctx_t *ctx);
static void direct_h264_pipeline_activate(app_ctx_t *ctx);
static void direct_h264_pipeline_deactivate(app_ctx_t *ctx);
static esp_err_t direct_h264_pipeline_drain(app_ctx_t *ctx, uint32_t timeout_ms);
static esp_err_t direct_h264_pipeline_submit(app_ctx_t *ctx,
                                             int8_t capture_buffer_index,
                                             uint32_t capture_sequence,
                                             int64_t capture_timestamp_us);
static void direct_h264_pipeline_encode_task(void *arg);
static void screen_preview_task(void *arg);
#if !CONFIG_JETKVM_SCREEN_BUTTONS
static void video_mode_button_task(void *arg);
#endif
static bool screen_preview_pipeline_submit(app_ctx_t *ctx,
                                           int8_t capture_buffer_index,
                                           uint32_t capture_sequence,
                                           size_t raw_size);
static esp_err_t screen_preview_pipeline_drain(app_ctx_t *ctx,
                                               uint32_t timeout_ms);

static const tc358743_capture_candidate_t s_candidates[] = {
    {
        .name = "colorbar594_2lane",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = true,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = false,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x07,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "rgb972_f20",
        .data_lane_num = 2,
        .output_rgb888 = true,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = false,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 972000000,
        .timing_preset = &s_csi_presets[2],
    },
    {
        .name = "rgb594_f20_test",
        .data_lane_num = 2,
        .output_rgb888 = true,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "rgb357_f20_auto_linux",
        .data_lane_num = 2,
        .output_rgb888 = true,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "yuv357_f20_auto_linux",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = false,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "rgb594_f20_auto_linux",
        .data_lane_num = 2,
        .output_rgb888 = true,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f20",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = false,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv1000_f20",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = false,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 1000000000,
        .timing_preset = &s_csi_presets[2],
    },
    {
        .name = "yuv1250_f20",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = false,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 1250000000,
        .timing_preset = &s_csi_presets[3],
    },
    {
        .name = "yuv594_1lane_f20_auto_linux",
        .data_lane_num = 1,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "colorbar594_1lane_f20_auto_linux",
        .data_lane_num = 1,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = true,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0xff,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv357_f20_hs05",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = false,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x05,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "yuv357_f20_hs14",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = false,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x14,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "yuv357_f20_hs15",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x15,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "yuv357_f25_hs05",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x05,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "yuv357_f25_hs14",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x14,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "yuv357_f25_hs15",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x15,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "yuv594_f20_hs07",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = false,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x07,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f20_hs07_line",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = false,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x07,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f20_hs16",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f20_hs17",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x17,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f25_hs07",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x07,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f25_hs16",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = true,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f25_hs17",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x17,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "colorbar357_f20_hs05",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = true,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x05,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "colorbar357_f25_hs15",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = true,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = true,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x15,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "colorbar357_f20_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = true,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x05,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "colorbar357_f25_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = true,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x15,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "rgb594_f20_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = true,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv357_f20_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x05,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "yuv357_f25_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x15,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 357000000,
        .timing_preset = &s_csi_presets[0],
    },
    {
        .name = "colorbar594_f20_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = true,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "colorbar594_f25_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = true,
        .use_colorbar = true,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f20_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = false,
        .use_colorbar = false,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_1lane_f20_sync_full_open",
        .data_lane_num = 1,
        .output_rgb888 = false,
        .byte_swap = true,
        .use_colorbar = false,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "colorbar594_1lane_f20_sync_full_open",
        .data_lane_num = 1,
        .output_rgb888 = false,
        .byte_swap = true,
        .use_colorbar = true,
        .use_full_timing = true,
        .use_extcnt = true,
        .bridge_has_hsync = true,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F20M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x00,
        .dtype_max = 0x3f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
    {
        .name = "yuv594_f25_sync_full_open",
        .data_lane_num = 2,
        .output_rgb888 = false,
        .byte_swap = true,
        .use_colorbar = false,
        .use_full_timing = false,
        .use_extcnt = true,
        .bridge_has_hsync = false,
        .disable_auto_mute = true,
        .continuous_clock = true,
        .hsckmd = false,
        .eot_disable = false,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_PLL_F25M,
        .hs_freq_sel_override = 0x16,
        .dtype_min = 0x12,
        .dtype_max = 0x2f,
        .lane_bps = 594000000,
        .timing_preset = &s_csi_presets[1],
    },
};

static uint32_t color_signature(const uint8_t *data, size_t len)
{
    uint32_t sig = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        sig ^= data[i];
        sig *= 16777619u;
    }
    return sig;
}

static void log_frame_buffer_probe(app_ctx_t *ctx, const char *stage)
{
    if (!ctx->frame_buffer || !ctx->frame_buffer_size) {
        return;
    }

    /* Registered capture buffers are kept cache-invalid and DMA-exclusive
     * across CSI -> H.264. A timeout does not prove that CSI has released the
     * current buffer, so probing even a few bytes here could race DMA and
     * would deliberately trip the fast-path's fail-closed overlap check. */
    for (uint8_t i = 0; i < ctx->capture_buffer_count; i++) {
        if (ctx->capture_buffers[i] == ctx->frame_buffer &&
            ctx->capture_cache_fastpaths[i] != DMA_CACHE_FASTPATH_INVALID_HANDLE) {
            ESP_LOGI(TAG, "fb-probe[%s] skipped: capture buffer %u is DMA-exclusive",
                     stage, (unsigned)i);
            return;
        }
    }

    size_t probe_len = ctx->frame_buffer_size < 64 ? ctx->frame_buffer_size : 64;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_cache_msync(ctx->frame_buffer, probe_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C));
    const uint8_t *buf = (const uint8_t *)ctx->frame_buffer;
    uint32_t sig = color_signature(buf, probe_len);

    ESP_LOGI(TAG,
             "fb-probe[%s] sig=0x%08" PRIx32 " bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
             stage, sig,
             probe_len > 0 ? buf[0] : 0,
             probe_len > 1 ? buf[1] : 0,
             probe_len > 2 ? buf[2] : 0,
             probe_len > 3 ? buf[3] : 0,
             probe_len > 4 ? buf[4] : 0,
             probe_len > 5 ? buf[5] : 0,
             probe_len > 6 ? buf[6] : 0,
             probe_len > 7 ? buf[7] : 0);
}

static cam_ctlr_color_t csi_effective_output_yuv_color(void)
{
    /* On P4 rev3 the paired IDI32 swaps restore the configured CSI byte
     * sequence in memory. A 16-bit-only swap changes UYVY <-> YUYV. */
    if (!TC_CSI_INPUT_16BIT_SWAP || TC_CSI_INPUT_8BIT_SWAP) {
        return TC_CSI_OUTPUT_YUV_COLOR;
    }

    switch (TC_CSI_OUTPUT_YUV_COLOR) {
    case CAM_CTLR_COLOR_YUV422_UYVY:
        return CAM_CTLR_COLOR_YUV422_YUYV;
    case CAM_CTLR_COLOR_YUV422_YUYV:
        return CAM_CTLR_COLOR_YUV422_UYVY;
    case CAM_CTLR_COLOR_YUV422_VYUY:
        return CAM_CTLR_COLOR_YUV422_YVYU;
    case CAM_CTLR_COLOR_YUV422_YVYU:
        return CAM_CTLR_COLOR_YUV422_VYUY;
    default:
        return TC_CSI_OUTPUT_YUV_COLOR;
    }
}

static bool csi_output_matches_h264_raw(esp_h264_raw_format_t fmt)
{
    cam_ctlr_color_t effective_color = csi_effective_output_yuv_color();

    return ((effective_color == CAM_CTLR_COLOR_YUV420) &&
            (fmt == ESP_H264_RAW_FMT_O_UYY_E_VYY)) ||
           ((effective_color == CAM_CTLR_COLOR_YUV422_UYVY) &&
            (fmt == ESP_H264_RAW_FMT_UYVY)) ||
           ((effective_color == CAM_CTLR_COLOR_YUV422_YUYV) &&
            (fmt == ESP_H264_RAW_FMT_YUYV));
}

/* Keep the compact DMA ring and raw-frame allocation under one policy.
 * The driver cannot derive the frame period from CSI geometry alone. */
static bool csi_use_compact_ring(const app_ctx_t *ctx,
                                 const tc358743_timings_t *timings,
                                 bool direct_h264_capture)
{
    return direct_h264_capture && !ctx->preview_only_active &&
           !ctx->use_driver_backup && !DIRECT_H264_OVERLAP_ENABLE &&
           timings->width == 1920U && timings->height == 1080U &&
           timings->fps == 30U;
}

/* The capture slots and the H.264 DB buffer are the video pool, allocated
 * once at boot. A mode uses the first slots it needs and the encoder takes the
 * DB buffer from esp_h264, so a rebuild neither frees nor requests them.
 * Release the encoder of another mode and report the PSRAM the rebuild still
 * allocates: 0, except the optional BGR888 diagnostic's private output. */
static esp_err_t video_mode_prepare(
    app_ctx_t *ctx, const tc358743_timings_t *timings,
    const tc358743_capture_candidate_t *candidate, size_t *bytes_out)
{
    if (ctx == NULL || timings == NULL || candidate == NULL ||
        bytes_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t width = candidate->use_full_timing ?
        timings->frame_width : timings->width;
    uint16_t height = candidate->use_full_timing ?
        timings->frame_height : timings->height;
    if (candidate->use_full_timing && HDMI_DEBUG_CAPTURE_ACTIVE_DE) {
        width = timings->width;
        height = timings->height;
    }
    if (width == 0U || height == 0U || width > FRAME_WIDTH_MAX ||
        height > FRAME_HEIGHT_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const bool direct_h264_capture = !candidate->output_rgb888 &&
        csi_output_matches_h264_raw(TC_H264_RAW_FORMAT);
    const bool direct_1080p30 = direct_h264_capture &&
        timings->width == 1920U && timings->height == 1080U &&
        timings->fps <= 30U;
    const size_t frame_bytes = candidate->output_rgb888 ?
        (size_t)width * height * 3U :
        (TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420 ?
            (size_t)width * height * 3U / 2U :
            (size_t)width * height * 2U);
    size_t capture_capacity = CSI_CAPTURE_CAPACITY(frame_bytes);
    if (direct_h264_capture) {
        capture_capacity = TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420 ?
            (size_t)FRAME_WIDTH_MAX * FRAME_HEIGHT_MAX * 3U / 2U :
            (size_t)FRAME_WIDTH_MAX * FRAME_HEIGHT_MAX * 2U;
    }
    const bool compact_csi_ring =
        csi_use_compact_ring(ctx, timings, direct_h264_capture);
    const size_t capture_count = ctx->use_driver_backup ? 1U :
        (compact_csi_ring ? CSI_FRAME_BUFFER_COUNT_COMPACT_1080P30 :
        (direct_h264_capture ?
            (direct_1080p30 ? CSI_FRAME_BUFFER_COUNT_DIRECT_1080P30 :
                              CSI_FRAME_BUFFER_COUNT_DIRECT_HIGH_RATE) :
            CSI_FRAME_BUFFER_COUNT_DEFAULT));
    if (!ctx->use_driver_backup) {
        /* Only the RGB888 debug candidates can need a larger slot. */
        if (capture_capacity > ctx->capture_buffers_capacity) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        for (size_t i = 0U; i < capture_count; ++i) {
            if (ctx->capture_buffers[i] == NULL) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    const bool prealloc_h264 = !ctx->preview_only_active &&
        (candidate->output_rgb888 ?
            (TC_H264_RAW_FORMAT == ESP_H264_RAW_FMT_BGR888) : true);
    if (!prealloc_h264 || !h264_hw_matches(ctx, width, height, timings->fps,
                                           TC_H264_RAW_FORMAT)) {
        h264_hw_deinit(ctx);
    }
    size_t working_set = 0U;
    if (prealloc_h264 && !ctx->h264_enc &&
        TC_H264_RAW_FORMAT == ESP_H264_RAW_FMT_BGR888) {
        /* Streaming output is borrowed from the already allocated WebRTC
         * pool. Only the standalone RGB diagnostic needs a private output. */
        const size_t raw_size =
            h264_raw_frame_size(width, height, TC_H264_RAW_FORMAT);
        working_set = raw_size / 8U;
        if (working_set < H264_OUTPUT_MIN_BYTES) {
            working_set = H264_OUTPUT_MIN_BYTES;
        }
    }
    /* Unless the capture goes straight into the pool's slots, csi_init()
     * leaves the CSI driver's backup buffer on, and the driver allocates it
     * in PSRAM: one frame of the output, at most 24 bits a pixel for RGB888
     * and 16 for YUV, rounded up as the driver rounds it. */
    if (!direct_h264_capture || ctx->use_driver_backup) {
        const size_t backup_frame = (size_t)width * height *
            (candidate->output_rgb888 ? 3U : 2U);
        working_set += (backup_frame + P4_CSI_BACKUP_BUFFER_ALIGN_BYTES - 1U) /
            P4_CSI_BACKUP_BUFFER_ALIGN_BYTES * P4_CSI_BACKUP_BUFFER_ALIGN_BYTES;
    }

    *bytes_out = working_set;
    return ESP_OK;
}

static void log_yuv422_frame_samples(const char *tag, const uint8_t *fb,
                                     uint16_t width, uint16_t height, size_t len)
{
    if (!fb || width < 2 || height == 0) {
        return;
    }

    size_t stride = (size_t)width * 2;
    uint16_t ys[] = {0, (uint16_t)(height / 2), (uint16_t)(height - 1)};
    uint16_t xs[] = {0, (uint16_t)(width / 4), (uint16_t)(width / 2),
                     (uint16_t)((width * 3) / 4), (uint16_t)(width - 2)};

    for (size_t yi = 0; yi < sizeof(ys) / sizeof(ys[0]); yi++) {
        uint16_t y = ys[yi];
        if (y >= height) {
            y = height - 1;
        }
        for (size_t xi = 0; xi < sizeof(xs) / sizeof(xs[0]); xi++) {
            uint16_t x = xs[xi] & (uint16_t)~1U;
            if (x >= width) {
                x = (width - 2) & (uint16_t)~1U;
            }
            size_t off = (size_t)y * stride + (size_t)x * 2;
            if (off + 8 > len) {
                continue;
            }
            const uint8_t *p = fb + off;
            ESP_LOGI(TAG,
                     "%s y=%u x=%u b=%02x %02x %02x %02x %02x %02x %02x %02x"
                     " asYUYV[y=%u/%u u=%u v=%u] asUYVY[y=%u/%u u=%u v=%u]",
                     tag, y, x,
                     p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                     p[0], p[2], p[1], p[3],
                     p[1], p[3], p[0], p[2]);
        }
    }
}

static void h264_hw_enable_clock(void)
{
    HP_SYS_CLKRST.soc_clk_ctrl1.reg_h264_sys_clk_en = 1;
    HP_SYS_CLKRST.peri_clk_ctrl26.reg_h264_clk_en = 1;
    HP_SYS_CLKRST.peri_clk_ctrl26.reg_h264_clk_src_sel = 1;
    esp_rom_delay_us(100);
}

/* The encoder task accounts for each frame in one critical section, and the
 * stats window takes its snapshot in the same section, so a frame's attempt,
 * encode time, maximum, output and timing samples fall in one window. */
static portMUX_TYPE s_video_metrics_mux = portMUX_INITIALIZER_UNLOCKED;

/* Since boot: a new mode or a restart of capture starts the metrics of
 * app_ctx_t again and leaves these as they are. Under s_video_metrics_mux. */
static video_encode_totals_t s_video_encode_totals;

static void video_metrics_add_encode_time(video_metrics_t *metrics,
                                          uint32_t encode_time_us)
{
    metrics->encode_attempts++;
    metrics->encode_time_us += encode_time_us;
    if (encode_time_us > metrics->encode_time_max_us) {
        metrics->encode_time_max_us = encode_time_us;
    }
}

/* An encode that produced no access unit to send. */
static void video_metrics_record_failed_encode(app_ctx_t *ctx,
                                               uint32_t encode_time_us,
                                               bool overflow)
{
    portENTER_CRITICAL(&s_video_metrics_mux);
    video_metrics_add_encode_time(&ctx->metrics, encode_time_us);
    ctx->metrics.encode_failures++;
    if (overflow) {
        ctx->metrics.encode_overflows++;
    }
    portEXIT_CRITICAL(&s_video_metrics_mux);
}

/* An encoded access unit, and whether the transport accepted it. */
static void video_metrics_record_frame(app_ctx_t *ctx,
                                       uint32_t encode_time_us,
                                       uint32_t capture_sequence,
                                       uint32_t length,
                                       bool keyframe,
                                       bool sink_accepted,
                                       int64_t csi_dma_eof_timestamp_us,
                                       int64_t h264_encode_start_timestamp_us)
{
    uint32_t handoff_us = 0;
    if (h264_encode_start_timestamp_us >= csi_dma_eof_timestamp_us) {
        uint64_t elapsed = (uint64_t)(h264_encode_start_timestamp_us -
                                      csi_dma_eof_timestamp_us);
        handoff_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    }

    video_metrics_t *metrics = &ctx->metrics;
    video_timing_window_t *window = &ctx->timing_window;
    portENTER_CRITICAL(&s_video_metrics_mux);
    video_metrics_add_encode_time(metrics, encode_time_us);
    if (metrics->last_encoded_capture_sequence &&
        capture_sequence > metrics->last_encoded_capture_sequence + 1) {
        metrics->capture_sequence_gaps +=
            capture_sequence - metrics->last_encoded_capture_sequence - 1;
    }
    metrics->last_encoded_capture_sequence = capture_sequence;
    metrics->encode_frames++;
    metrics->encoded_bytes += length;
    if (keyframe) {
        metrics->encode_keyframes++;
    }
    if (sink_accepted) {
        metrics->sink_frames++;
    } else {
        metrics->sink_drops++;
    }
    if (window->handoff_count < VIDEO_TIMING_WINDOW_SAMPLES) {
        window->handoff_us[window->handoff_count++] = handoff_us;
    } else {
        window->dropped_samples++;
    }
    uint32_t *encode_samples = keyframe ? window->idr_encode_us :
                                          window->p_encode_us;
    uint16_t *encode_count = keyframe ? &window->idr_encode_count :
                                        &window->p_encode_count;
    if (*encode_count < VIDEO_TIMING_WINDOW_SAMPLES) {
        encode_samples[(*encode_count)++] = encode_time_us;
    } else {
        window->dropped_samples++;
    }
    video_encode_totals_add(&s_video_encode_totals,
                            h264_encode_start_timestamp_us + encode_time_us,
                            keyframe, encode_time_us, handoff_us);
    portEXIT_CRITICAL(&s_video_metrics_mux);
}

void jetkvm_video_get_encode_stats(webrtc_transport_video_encode_t *encode)
{
    portENTER_CRITICAL(&s_video_metrics_mux);
    const video_encode_totals_t totals = s_video_encode_totals;
    portEXIT_CRITICAL(&s_video_metrics_mux);
    const video_encode_longest_t longest =
        video_encode_totals_longest(&totals, esp_timer_get_time());
    *encode = (webrtc_transport_video_encode_t) {
        .p_frames = totals.p_frames,
        .p_encode_us = totals.p_encode_us,
        .p_encode_max_us = longest.p_encode_us,
        .idr_frames = totals.idr_frames,
        .idr_encode_us = totals.idr_encode_us,
        .idr_encode_max_us = longest.idr_encode_us,
        .handoff_us = totals.handoff_us,
        .handoff_max_us = longest.handoff_us,
    };
}

static void video_metrics_begin_mode(app_ctx_t *ctx, uint16_t width, uint16_t height, uint16_t fps)
{
    memset(&ctx->metrics, 0, sizeof(ctx->metrics));
    memset(&ctx->timing_window, 0, sizeof(ctx->timing_window));
    ctx->metrics.mode_width = width;
    ctx->metrics.mode_height = height;
    ctx->metrics.mode_fps = fps;
    ctx->metrics.window_start_us = esp_timer_get_time();
    ctx->metrics.window_http_drops = ctx->h264_stream_dropped;
    ctx->capture_sequence = 0;
    ctx->current_capture_sequence = 0;
    ctx->current_capture_timestamp_us = 0;
    jetkvm_screen_stats_t screen_stats = {0};
    if (jetkvm_screen_get_stats(&screen_stats) == ESP_OK) {
        ctx->screen_preview_last_logged_applied =
            screen_stats.preview_frames_applied;
        ctx->screen_preview_last_logged_ppa_frames =
            screen_stats.preview_ppa_frames;
        ctx->screen_preview_last_logged_ppa_total_us =
            screen_stats.preview_ppa_total_us;
    }
}

static void video_metrics_maybe_log(app_ctx_t *ctx)
{
    video_metrics_t *metrics = &ctx->metrics;
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - metrics->window_start_us;
    if (!metrics->window_start_us || elapsed_us < VIDEO_STATS_INTERVAL_US) {
        return;
    }

    /* One copy of the counters for both the rates and the next window. The
     * log lines below take tens of milliseconds on the console; frames
     * encoded while they print belong to the next window. Reading the live
     * counters at the reset left them out of every window, so encode_fps read
     * about one frame per second low. The maximum and the timing samples
     * start over at the same point. */
    video_metrics_t snap;
    video_timing_window_t timing;
    portENTER_CRITICAL(&s_video_metrics_mux);
    snap = *metrics;
    metrics->encode_time_max_us = 0;
    timing = ctx->timing_window;
    memset(&ctx->timing_window, 0, sizeof(ctx->timing_window));
    portEXIT_CRITICAL(&s_video_metrics_mux);
    const uint32_t http_drops = ctx->h264_stream_dropped;

    uint32_t capture_frames = snap.capture_frames;
    uint32_t capture_nofree_drops = snap.capture_nofree_drops;
    uint32_t delta_capture = capture_frames - metrics->window_capture_frames;
    uint32_t delta_nofree = capture_nofree_drops - metrics->window_capture_nofree_drops;
    uint32_t delta_stale = snap.capture_stale_drops - metrics->window_capture_stale_drops;
    uint32_t delta_sequence_gaps = snap.capture_sequence_gaps - metrics->window_capture_sequence_gaps;
    uint32_t delta_encode_attempts = snap.encode_attempts - metrics->window_encode_attempts;
    uint32_t delta_encode_frames = snap.encode_frames - metrics->window_encode_frames;
    uint32_t delta_encode_failures = snap.encode_failures - metrics->window_encode_failures;
    uint32_t delta_output_skips = snap.encode_output_skips - metrics->window_encode_output_skips;
    uint32_t delta_encode_overflows = snap.encode_overflows - metrics->window_encode_overflows;
    uint32_t delta_sink_frames = snap.sink_frames - metrics->window_sink_frames;
    uint32_t delta_sink_drops = snap.sink_drops - metrics->window_sink_drops;
    uint64_t delta_encoded_bytes = snap.encoded_bytes - metrics->window_encoded_bytes;
    uint64_t delta_encode_time_us = snap.encode_time_us - metrics->window_encode_time_us;
    uint32_t delta_direct_replaced =
        snap.direct_ready_replaced - metrics->window_direct_ready_replaced;
    uint32_t delta_direct_expired =
        snap.direct_expired_drops - metrics->window_direct_expired_drops;
    uint32_t delta_http_drops = http_drops - metrics->window_http_drops;
    uint32_t capture_fps_x100 = video_rate_x100(delta_capture, elapsed_us);
    uint32_t encode_fps_x100 = video_rate_x100(delta_encode_frames, elapsed_us);
    uint32_t sink_fps_x100 = video_rate_x100(delta_sink_frames, elapsed_us);
    uint32_t bitrate_kbps = (uint32_t)((delta_encoded_bytes * 8U * 1000U) / (uint64_t)elapsed_us);
    uint32_t encode_avg_us = delta_encode_attempts ?
                             (uint32_t)(delta_encode_time_us / delta_encode_attempts) : 0;
    uint32_t handoff_avg_us = video_timing_average(
        timing.handoff_us, timing.handoff_count);
    uint32_t handoff_max_us = video_timing_maximum(
        timing.handoff_us, timing.handoff_count);
    uint32_t p_encode_avg_us = video_timing_average(
        timing.p_encode_us, timing.p_encode_count);
    uint32_t p_encode_max_us = video_timing_maximum(
        timing.p_encode_us, timing.p_encode_count);
    uint32_t idr_encode_avg_us = video_timing_average(
        timing.idr_encode_us, timing.idr_encode_count);
    uint32_t idr_encode_max_us = video_timing_maximum(
        timing.idr_encode_us, timing.idr_encode_count);
    uint32_t handoff_p50_us = video_timing_percentile(
        timing.handoff_us, timing.handoff_count, 50);
    uint32_t handoff_p95_us = video_timing_percentile(
        timing.handoff_us, timing.handoff_count, 95);
    uint32_t p_encode_p50_us = video_timing_percentile(
        timing.p_encode_us, timing.p_encode_count, 50);
    uint32_t p_encode_p95_us = video_timing_percentile(
        timing.p_encode_us, timing.p_encode_count, 95);
    uint32_t idr_encode_p50_us = video_timing_percentile(
        timing.idr_encode_us, timing.idr_encode_count, 50);
    uint32_t idr_encode_p95_us = video_timing_percentile(
        timing.idr_encode_us, timing.idr_encode_count, 95);

    ESP_LOGI(TAG,
             "VIDEO_STATS mode=%ux%u@%u interval_ms=%" PRId64
             " capture_fps=%" PRIu32 ".%02" PRIu32
             " encode_fps=%" PRIu32 ".%02" PRIu32
             " sink_fps=%" PRIu32 ".%02" PRIu32
             " bitrate_kbps=%" PRIu32 " capture_total=%" PRIu32 " encode_total=%" PRIu32,
             metrics->mode_width, metrics->mode_height, metrics->mode_fps, elapsed_us / 1000,
             capture_fps_x100 / 100, capture_fps_x100 % 100,
             encode_fps_x100 / 100, encode_fps_x100 % 100,
             sink_fps_x100 / 100, sink_fps_x100 % 100,
             bitrate_kbps, capture_frames, snap.encode_frames);
    ESP_LOGI(TAG,
             "VIDEO_DROPS mode=%ux%u@%u nofree=%" PRIu32 " stale=%" PRIu32
             " sequence_gap=%" PRIu32 " encode_fail=%" PRIu32
             " encode_overflow=%" PRIu32 " sink=%" PRIu32 " http=%" PRIu32
             " direct_replaced=%" PRIu32 " direct_expired=%" PRIu32
             " encode_avg_us=%" PRIu32 " encode_max_us=%" PRIu32 " keyframes=%" PRIu32
             " no_output=%" PRIu32,
             metrics->mode_width, metrics->mode_height, metrics->mode_fps,
             delta_nofree, delta_stale, delta_sequence_gaps, delta_encode_failures,
             delta_encode_overflows, delta_sink_drops, delta_http_drops,
             delta_direct_replaced, delta_direct_expired,
             encode_avg_us, snap.encode_time_max_us, snap.encode_keyframes,
             delta_output_skips);
    ESP_LOGI(TAG,
             "VIDEO_TIMING mode=%ux%u@%u"
             " handoff_count=%u handoff_avg_us=%" PRIu32
             " handoff_p50_us=%" PRIu32 " handoff_p95_us=%" PRIu32
             " handoff_max_us=%" PRIu32
             " p_count=%u p_avg_us=%" PRIu32 " p_p50_us=%" PRIu32
             " p_p95_us=%" PRIu32 " p_max_us=%" PRIu32
             " idr_count=%u idr_avg_us=%" PRIu32
             " idr_p50_us=%" PRIu32 " idr_p95_us=%" PRIu32
             " idr_max_us=%" PRIu32 " sample_drops=%" PRIu32,
             metrics->mode_width, metrics->mode_height, metrics->mode_fps,
             timing.handoff_count, handoff_avg_us, handoff_p50_us,
             handoff_p95_us, handoff_max_us,
             timing.p_encode_count, p_encode_avg_us, p_encode_p50_us,
             p_encode_p95_us, p_encode_max_us,
             timing.idr_encode_count, idr_encode_avg_us, idr_encode_p50_us,
             idr_encode_p95_us, idr_encode_max_us, timing.dropped_samples);
    jetkvm_screen_stats_t screen_stats = {0};
    if (jetkvm_screen_get_stats(&screen_stats) == ESP_OK) {
        uint32_t delta_applied = screen_stats.preview_frames_applied -
                                 ctx->screen_preview_last_logged_applied;
        uint32_t preview_fps_x100 = video_rate_x100(delta_applied, elapsed_us);
        uint32_t delta_ppa_frames = screen_stats.preview_ppa_frames -
            ctx->screen_preview_last_logged_ppa_frames;
        uint32_t delta_ppa_total_us = screen_stats.preview_ppa_total_us -
            ctx->screen_preview_last_logged_ppa_total_us;
        uint32_t ppa_avg_us = delta_ppa_frames != 0U ?
            delta_ppa_total_us / delta_ppa_frames : 0U;
        uint32_t ppa_capacity_fps_x100 = ppa_avg_us != 0U ?
            100000000U / ppa_avg_us : 0U;
        ESP_LOGI(TAG,
                 "SCREEN_PREVIEW active=%d input=%ux%u@%u"
                 " refresh_fps=%" PRIu32 ".%02" PRIu32
                 " ppa_avg_us=%" PRIu32 " ppa_max_us=%" PRIu32
                 " ppa_capacity_fps=%" PRIu32 ".%02" PRIu32
                 " applied=%" PRIu32 " submitted=%" PRIu32
                 " dropped=%" PRIu32,
                 screen_stats.preview_active, screen_stats.preview_width,
                 screen_stats.preview_height, screen_stats.preview_fps,
                 preview_fps_x100 / 100U, preview_fps_x100 % 100U,
                 ppa_avg_us, screen_stats.preview_ppa_max_us,
                 ppa_capacity_fps_x100 / 100U,
                 ppa_capacity_fps_x100 % 100U,
                 screen_stats.preview_frames_applied,
                 screen_stats.preview_frames_submitted,
                 screen_stats.preview_frames_dropped);
        ctx->screen_preview_last_logged_applied =
            screen_stats.preview_frames_applied;
        ctx->screen_preview_last_logged_ppa_frames =
            screen_stats.preview_ppa_frames;
        ctx->screen_preview_last_logged_ppa_total_us =
            screen_stats.preview_ppa_total_us;
    }
    if (!metrics->last_heap_log_us ||
        now_us - metrics->last_heap_log_us >= VIDEO_HEAP_INTERVAL_US) {
        dma_cache_fastpath_global_stats_t cache_stats = {0};
        dma_cache_fastpath_get_global_stats(&cache_stats);
        /* A largest-block query walks the whole allocator with interrupts
         * masked. Keep periodic video logging to constant-time counters;
         * exact fragmentation remains available in explicit diagnostics. */
        ESP_LOGI(TAG,
                 "VIDEO_HEAP int_free=%u int_min=%u"
                 " spiram_free=%u spiram_min=%u stack_hwm=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG,
                 "CACHE_FASTPATH c2m_calls=%" PRIu64 " c2m_bytes=%" PRIu64
                 " m2c_calls=%" PRIu64 " m2c_bytes=%" PRIu64
                 " passthrough=%" PRIu64 " unexpected_overlap=%" PRIu64,
                 cache_stats.skipped_c2m_calls,
                 cache_stats.skipped_c2m_bytes,
                 cache_stats.skipped_m2c_calls,
                 cache_stats.skipped_m2c_bytes,
                 cache_stats.passthrough_calls,
                 cache_stats.unexpected_overlap_calls);
        metrics->last_heap_log_us = now_us;
    }

    metrics->window_start_us = now_us;
    metrics->window_capture_frames = capture_frames;
    metrics->window_capture_nofree_drops = capture_nofree_drops;
    metrics->window_capture_stale_drops = snap.capture_stale_drops;
    metrics->window_capture_sequence_gaps = snap.capture_sequence_gaps;
    metrics->window_encode_attempts = snap.encode_attempts;
    metrics->window_encode_frames = snap.encode_frames;
    metrics->window_encode_failures = snap.encode_failures;
    metrics->window_encode_output_skips = snap.encode_output_skips;
    metrics->window_encode_overflows = snap.encode_overflows;
    metrics->window_sink_frames = snap.sink_frames;
    metrics->window_sink_drops = snap.sink_drops;
    metrics->window_encoded_bytes = snap.encoded_bytes;
    metrics->window_encode_time_us = snap.encode_time_us;
    metrics->window_direct_ready_replaced = snap.direct_ready_replaced;
    metrics->window_direct_expired_drops = snap.direct_expired_drops;
    metrics->window_http_drops = http_drops;
}

static void h264_sync_idr_arm(app_ctx_t *ctx)
{
    if (!ctx || !ctx->h264_enc || ctx->h264_sync_idr_generation != 0) {
        return;
    }
    uint32_t peer_generation = 0;
    if (!webrtc_transport_take_sync_idr_request(&peer_generation)) {
        return;
    }
    esp_h264_enc_param_hw_handle_t param = NULL;
    esp_h264_err_t ret = esp_h264_enc_hw_get_param_hd(ctx->h264_enc, &param);
    if (ret == ESP_H264_ERR_OK) {
        ret = esp_h264_enc_force_idr(&param->base);
    }
    if (ret != ESP_H264_ERR_OK) {
        webrtc_transport_complete_sync_idr_production(peer_generation, false);
        ESP_LOGW(TAG, "sync IDR request failed generation=%" PRIu32 " ret=%d",
                 peer_generation, ret);
        return;
    }
    ctx->h264_sync_idr_generation = peer_generation;
    ESP_LOGI(TAG, "sync IDR armed generation=%" PRIu32 " gop=%u",
             peer_generation, ctx->h264_gop);
}

static void h264_sync_idr_production_complete(app_ctx_t *ctx, bool success)
{
    if (!ctx || ctx->h264_sync_idr_generation == 0) {
        return;
    }
    uint32_t peer_generation = ctx->h264_sync_idr_generation;
    ctx->h264_sync_idr_generation = 0;
    webrtc_transport_complete_sync_idr_production(peer_generation, success);
    if (success) {
        ESP_LOGI(TAG, "sync IDR produced and accepted generation=%" PRIu32,
                 peer_generation);
    } else {
        ESP_LOGW(TAG, "sync IDR failed generation=%" PRIu32,
                 peer_generation);
    }
}

static void h264_hw_deinit(app_ctx_t *ctx)
{
    h264_sync_idr_production_complete(ctx, false);

    if (ctx->h264_enc) {
        esp_h264_enc_close(ctx->h264_enc);
        esp_h264_enc_del(ctx->h264_enc);
        ctx->h264_enc = NULL;
    }
    if (ctx->h264_output_buf) {
        heap_caps_free(ctx->h264_output_buf);
        ctx->h264_output_buf = NULL;
    }
    ctx->h264_output_len = 0;
    ctx->h264_width = 0;
    ctx->h264_height = 0;
    ctx->h264_fps = 0;
    ctx->h264_gop = 0;
    ctx->h264_bitrate = 0;
    ctx->h264_pic_type = 0;
}

static uint8_t h264_mode_fps(uint16_t width, uint16_t height, uint16_t detected_fps)
{
    if (width == 1920 && height == 1080 && detected_fps >= 28 && detected_fps <= 32) {
        return 30;
    }
    if (width == 1280 && height == 720 && detected_fps >= 28 && detected_fps <= 32) {
        return 30;
    }
    if (detected_fps == 0) {
        return 30;
    }
    return detected_fps > UINT8_MAX ? UINT8_MAX : (uint8_t)detected_fps;
}

static uint32_t h264_target_bitrate(uint16_t width, uint16_t height,
                                    uint8_t fps, uint32_t quality_permille)
{
    uint32_t bitrate = (uint32_t)(((uint64_t)width * height * fps * 4U) / 30U);
    if (width == 1920 && height == 1080 && fps == 30) {
        bitrate = H264_1080P_TARGET_BITRATE_BPS;
    }
    /* Auto adapts below the High rate; its ceiling is High. */
    if (quality_permille == JETKVM_VIDEO_QUALITY_AUTO_PERMILLE) {
        quality_permille = JETKVM_VIDEO_QUALITY_MAX_PERMILLE;
    }
    bitrate = (uint32_t)(((uint64_t)bitrate * quality_permille + 500U) /
                         1000U);
    if (bitrate < 1000000) {
        bitrate = 1000000;
    } else if (bitrate > H264_TARGET_BITRATE_MAX_BPS) {
        bitrate = H264_TARGET_BITRATE_MAX_BPS;
    }
    return bitrate;
}

static bool h264_hw_update_quality(app_ctx_t *ctx)
{
    const uint32_t quality_permille = jetkvm_video_quality_get_permille();
    const uint32_t ceiling = h264_target_bitrate(
        ctx->h264_width, ctx->h264_height, ctx->h264_fps, quality_permille);
    /* Only Auto follows receiver feedback; a manual preset is a fixed rate. */
    const bool adaptive =
        quality_permille == JETKVM_VIDEO_QUALITY_AUTO_PERMILLE;
    const uint32_t bitrate = webrtc_transport_video_bitrate(ceiling, adaptive);
    if (bitrate == ctx->h264_bitrate) {
        return true;
    }

    /* Only the H.264 worker calls this, between jobs. The SDK updates RC
     * under the encoder's parameter mutex; no reference, GOP, CSI buffer or
     * peer lifecycle changes are needed for a bitrate-only preference. */
    esp_h264_enc_param_hw_handle_t param = NULL;
    esp_h264_err_t ret = esp_h264_enc_hw_get_param_hd(ctx->h264_enc, &param);
    if (ret == ESP_H264_ERR_OK) {
        ret = esp_h264_enc_set_bitrate(&param->base, bitrate);
    }
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "h264hw: bitrate update failed requested=%" PRIu32
                 " applied=%" PRIu32 " ret=%d",
                 bitrate, ctx->h264_bitrate, ret);
        return false;
    }
    const uint32_t previous_bitrate = ctx->h264_bitrate;
    ctx->h264_bitrate = bitrate;
    ESP_LOGI(TAG, "h264hw: bitrate updated old=%" PRIu32 " new=%" PRIu32
             " quality=%.3f", previous_bitrate, bitrate,
             (double)quality_permille / 1000.0);
    return true;
}

static bool h264_hw_matches(const app_ctx_t *ctx, uint16_t width,
                            uint16_t height, uint16_t fps,
                            esp_h264_raw_format_t pic_type)
{
    /* GOP derives from the configured frame rate. */
    return ctx->h264_enc && ctx->h264_width == width &&
           ctx->h264_height == height &&
           ctx->h264_fps == h264_mode_fps(width, height, fps) &&
           ctx->h264_pic_type == pic_type;
}

/* Keyframes per second. Upstream sends two (GOP = fps / 2). On the Wi-Fi
 * board a 100-160 KB keyframe takes 100-200 ms to arrive, a visible pause
 * each time, so it sends one. */
#if JETKVM_BOARD_WIFI
#define H264_KEYFRAMES_PER_SECOND 1U
#else
#define H264_KEYFRAMES_PER_SECOND 2U
#endif

static esp_err_t h264_hw_ensure(app_ctx_t *ctx, uint16_t width, uint16_t height,
                                uint16_t fps, esp_h264_raw_format_t pic_type)
{
    uint8_t configured_fps = h264_mode_fps(width, height, fps);
    /* As JetKVM native/cgo, scale GOP with frame rate so the IDR cadence is
     * the same for every accepted mode. */
    uint8_t configured_gop = configured_fps > 0 ?
                                 configured_fps / H264_KEYFRAMES_PER_SECOND : 30;
    if (configured_gop < 1) {
        configured_gop = 1;
    }
    const uint32_t quality_permille = jetkvm_video_quality_get_permille();
    const uint32_t bitrate = h264_target_bitrate(
        width, height, configured_fps, quality_permille);

    if (h264_hw_matches(ctx, width, height, fps, pic_type)) {
        return ESP_OK;
    }

    if (!ESP_H264_HW_IS_SUPPORTED_PIC_TYPE(pic_type)) {
        ESP_LOGE(TAG, "h264hw: %s is not supported by HW encoder", h264_raw_fmt_name(pic_type));
        return ESP_ERR_NOT_SUPPORTED;
    }

    h264_hw_deinit(ctx);
    h264_hw_enable_clock();

    size_t raw_size = h264_raw_frame_size(width, height, pic_type);
    /* Keep the same per-AU capacity and overflow handling. Normal streaming
     * borrows this storage from WebRTC instead of allocating another copy. */
    size_t output_capacity = raw_size / 8;
    if (output_capacity < H264_OUTPUT_MIN_BYTES) {
        output_capacity = H264_OUTPUT_MIN_BYTES;
    }

    esp_h264_enc_cfg_hw_t hw_cfg = {
        .pic_type = pic_type,
        .gop = configured_gop,
        .fps = configured_fps,
        .res = { .width = width, .height = height },
        .rc = {
            .bitrate = bitrate,
            .qp_min = H264_QUALITY_QP_MIN,
            .qp_max = ESP_H264_QP_MAX,
        },
    };

    ESP_LOGI(TAG, "h264hw: init %ux%u@%u %s raw=%u out_cap=%u gop=%u bitrate=%" PRIu32 " quality=%.3f",
             width, height, configured_fps, h264_raw_fmt_name(pic_type),
             (unsigned)raw_size, (unsigned)output_capacity, configured_gop,
             bitrate, (double)quality_permille / 1000.0);
    ESP_LOGI(TAG,
             "h264hw: heap before new int_free=%u int_largest=%u sp_free=%u sp_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    esp_h264_enc_handle_t hw_enc = NULL;
    esp_h264_err_t hr = esp_h264_enc_hw_new(&hw_cfg, &hw_enc);
    if (hr != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "h264hw: new failed: %d", hr);
        return hr == ESP_H264_ERR_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    hr = esp_h264_enc_open(hw_enc);
    if (hr != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "h264hw: open failed: %d", hr);
        esp_h264_enc_del(hw_enc);
        return hr == ESP_H264_ERR_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    ctx->h264_output_len = output_capacity;
    if (pic_type == ESP_H264_RAW_FMT_BGR888) {
        /* The optional one-shot RGB serial diagnostic has no network sink. */
        ctx->h264_output_buf = heap_caps_aligned_alloc(
            128, ctx->h264_output_len, MALLOC_CAP_SPIRAM);
        if (!ctx->h264_output_buf) {
            ESP_LOGE(TAG, "h264hw: diagnostic output alloc failed (%" PRIu32 " bytes)",
                     ctx->h264_output_len);
            esp_h264_enc_close(hw_enc);
            esp_h264_enc_del(hw_enc);
            ctx->h264_output_len = 0;
            return ESP_ERR_NO_MEM;
        }
    }

    ctx->h264_enc = hw_enc;
    ctx->h264_width = width;
    ctx->h264_height = height;
    ctx->h264_fps = configured_fps;
    ctx->h264_gop = configured_gop;
    ctx->h264_bitrate = bitrate;
    ctx->h264_pic_type = pic_type;
    ctx->h264_serial_dumped = false;
    ESP_LOGI(TAG, "h264hw: ready %ux%u@%u %s gop=%u bitrate=%" PRIu32 " out=%" PRIu32,
             width, height, configured_fps, h264_raw_fmt_name(pic_type),
             configured_gop, bitrate, ctx->h264_output_len);
    return ESP_OK;
}

static void p4_override_hs_freq_sel(uint8_t hs_freq_sel)
{
    csi_host_dev_t *host = &MIPI_CSI_HOST;

    mipi_csi_phy_ll_write_clock(host, 0, false);
    mipi_csi_phy_ll_write_reg_addr(host, 0x44);
    mipi_csi_phy_ll_write_clock(host, 1, false);
    mipi_csi_phy_ll_write_clock(host, 0, false);
    mipi_csi_phy_ll_write_reg_val(host, hs_freq_sel << 1);
    mipi_csi_phy_ll_write_clock(host, 1, false);
    mipi_csi_phy_ll_write_clock(host, 0, false);
}

static portMUX_TYPE s_csi_buffer_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_direct_h264_pipeline_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t csi_all_buffer_mask(const app_ctx_t *ctx)
{
    return ctx->capture_buffer_count >= 32 ? UINT32_MAX : ((1u << ctx->capture_buffer_count) - 1u);
}

static int8_t IRAM_ATTR csi_buffer_index_from_ptr(app_ctx_t *ctx, const void *buffer)
{
    if (!buffer) {
        return -1;
    }
    for (uint8_t i = 0; i < ctx->capture_buffer_count; i++) {
        if (ctx->capture_buffers[i] == buffer) {
            return (int8_t)i;
        }
    }
    return -1;
}

static void csi_reset_buffer_state(app_ctx_t *ctx)
{
    ctx->capture_next_buffer = 0;
    ctx->free_buffers = csi_all_buffer_mask(ctx);
    ctx->inflight_buffers = 0;
    ctx->ready_buffers = 0;
    ctx->locked_buffers = 0;
    ctx->csi_no_free_buffers = 0;
    memset((void *)ctx->capture_received_sizes, 0, sizeof(ctx->capture_received_sizes));
    memset((void *)ctx->capture_buffer_sequences, 0, sizeof(ctx->capture_buffer_sequences));
    memset((void *)ctx->capture_buffer_timestamps_us, 0, sizeof(ctx->capture_buffer_timestamps_us));
}

static int8_t csi_take_ready_buffer(app_ctx_t *ctx, size_t *received,
                                    uint32_t *capture_sequence,
                                    int64_t *capture_timestamp_us,
                                    uint32_t *stale_dropped)
{
    int8_t index = -1;
    size_t size = 0;
    uint32_t sequence = 0;
    int64_t timestamp_us = 0;
    uint32_t dropped = 0;

    portENTER_CRITICAL(&s_csi_buffer_mux);
    uint32_t ready = ctx->ready_buffers & csi_all_buffer_mask(ctx);
    if (ready) {
        /* Always consume the newest completed DMA frame. Release every older
         * ready frame now so it cannot be encoded out of order and so CSI has
         * buffers available while the synchronous hardware encoder is busy. */
        for (uint8_t i = 0; i < ctx->capture_buffer_count; i++) {
            if ((ready & (1u << i)) &&
                (index < 0 || ctx->capture_buffer_sequences[i] > sequence)) {
                index = (int8_t)i;
                sequence = ctx->capture_buffer_sequences[i];
            }
        }

        if (index >= 0) {
            uint32_t bit = 1u << (uint8_t)index;
            uint32_t stale = ready & ~bit;
            for (uint8_t i = 0; i < ctx->capture_buffer_count; i++) {
                uint32_t stale_bit = 1u << i;
                if (stale & stale_bit) {
                    ctx->ready_buffers &= ~stale_bit;
                    ctx->free_buffers |= stale_bit;
                    ctx->capture_received_sizes[i] = 0;
                    ctx->capture_buffer_sequences[i] = 0;
                    ctx->capture_buffer_timestamps_us[i] = 0;
                    dropped++;
                }
            }
            ctx->ready_buffers &= ~bit;
            ctx->locked_buffers |= bit;
            ctx->frame_buffer = ctx->capture_buffers[index];
            size = ctx->capture_received_sizes[index];
            sequence = ctx->capture_buffer_sequences[index];
            timestamp_us = ctx->capture_buffer_timestamps_us[index];
            ctx->last_received_size = size;
            ctx->metrics.capture_stale_drops += dropped;
        }
    }
    portEXIT_CRITICAL(&s_csi_buffer_mux);

    if (received) {
        *received = size;
    }
    if (capture_sequence) {
        *capture_sequence = sequence;
    }
    if (capture_timestamp_us) {
        *capture_timestamp_us = timestamp_us;
    }
    if (stale_dropped) {
        *stale_dropped = dropped;
    }
    return index;
}

static void csi_release_consumed_buffer(app_ctx_t *ctx, int8_t index)
{
    if (index < 0 || (uint8_t)index >= ctx->capture_buffer_count) {
        return;
    }

    uint32_t bit = 1u << (uint8_t)index;
    portENTER_CRITICAL(&s_csi_buffer_mux);
    ctx->locked_buffers &= ~bit;
    ctx->free_buffers |= bit;
    ctx->capture_received_sizes[(uint8_t)index] = 0;
    ctx->capture_buffer_sequences[(uint8_t)index] = 0;
    ctx->capture_buffer_timestamps_us[(uint8_t)index] = 0;
    portEXIT_CRITICAL(&s_csi_buffer_mux);
}

static void IRAM_ATTR csi_release_consumed_buffer_from_isr(app_ctx_t *ctx, int8_t index)
{
    if (index < 0 || (uint8_t)index >= ctx->capture_buffer_count) {
        return;
    }

    uint32_t bit = 1u << (uint8_t)index;
    portENTER_CRITICAL_ISR(&s_csi_buffer_mux);
    ctx->locked_buffers &= ~bit;
    ctx->free_buffers |= bit;
    ctx->capture_received_sizes[(uint8_t)index] = 0;
    ctx->capture_buffer_sequences[(uint8_t)index] = 0;
    ctx->capture_buffer_timestamps_us[(uint8_t)index] = 0;
    portEXIT_CRITICAL_ISR(&s_csi_buffer_mux);
}

static void h264_pipeline_encode_one(app_ctx_t *ctx,
                                     uint8_t *raw_buffer,
                                     size_t raw_size,
                                     uint32_t capture_sequence,
                                     int64_t capture_timestamp_us,
                                     const char *path_name,
                                     int8_t overlap_buffer_index,
                                     uint32_t overlap_generation)
{
    if (!ctx->h264_enc || !raw_buffer || raw_size == 0) {
        ctx->metrics.encode_failures++;
        return;
    }
    /* From acceptance on, OS Services guest video owns the WebRTC video. */
    if (jetkvm_video_guest_owns_display()) {
        ctx->metrics.encode_output_skips++;
        return;
    }
    if (!h264_hw_update_quality(ctx)) {
        /* No encode or output reservation has started. Keep the applied
         * value and references intact, and retry the preference next job. */
        ctx->metrics.encode_failures++;
        return;
    }

    esp_h264_enc_in_frame_t in_frame = {
        .raw_data = {
            .buffer = raw_buffer,
            .len = raw_size,
        },
        .pts = capture_timestamp_us,
    };
    /* Encode into an existing transport slot while it is exclusively owned
     * by this task. Publishing transfers ownership only after DMA completes;
     * it does not add a capture buffer or another queued frame. */
    webrtc_transport_au_reservation_t rtc_output = {0};
    bool rtc_reserved = webrtc_transport_reserve_access_unit(
        ctx->h264_width, ctx->h264_height, ctx->h264_fps, &rtc_output);
    if (!rtc_reserved) {
        /* reserve_access_unit publishes the mode even without a subscriber.
         * With no consumer or no free output, return the raw frame to CSI
         * without advancing H.264 references or consuming a pending IDR. */
        ctx->metrics.encode_output_skips++;
        return;
    }
    if (rtc_output.capacity < ctx->h264_output_len) {
        webrtc_transport_cancel_access_unit(&rtc_output);
        ctx->metrics.encode_failures++;
        return;
    }
    esp_h264_enc_out_frame_t out_frame = {
        .raw_data = {
            .buffer = rtc_output.data,
            .len = ctx->h264_output_len,
        },
    };

    h264_sync_idr_arm(ctx);
    int64_t start_us = esp_timer_get_time();
    esp_h264_err_t hr = esp_h264_enc_process(ctx->h264_enc,
                                              &in_frame, &out_frame);
    int64_t end_us = esp_timer_get_time();
    uint32_t encode_time_us = (uint32_t)(end_us - start_us);

    int64_t capture_eof_timestamp_us = capture_timestamp_us;
    bool overlap_complete = overlap_buffer_index < 0;
    if (overlap_buffer_index >= 0) {
        int64_t deadline_us = esp_timer_get_time() +
                              DIRECT_H264_OVERLAP_EOF_WAIT_US;
        while (esp_timer_get_time() < deadline_us) {
            portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
            const direct_h264_slot_t *slot =
                &ctx->direct_h264_slots[(uint8_t)overlap_buffer_index];
            if (slot->generation == overlap_generation &&
                slot->capture_complete && !slot->cancelled) {
                capture_sequence = slot->capture_sequence;
                capture_eof_timestamp_us =
                    slot->csi_dma_eof_timestamp_us;
                overlap_complete = true;
            }
            portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
            if (overlap_complete) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        /* If H.264 reached frame-done before CSI reached EOF, it necessarily
         * caught the producer near the bottom of the image.  Never publish a
         * potentially torn access unit; the lead must be increased instead. */
        if (!overlap_complete || end_us < capture_eof_timestamp_us) {
            portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
            direct_h264_slot_t *slot =
                &ctx->direct_h264_slots[(uint8_t)overlap_buffer_index];
            if (slot->generation == overlap_generation) {
                /* A late EOF must fail the overlap handoff and release the
                 * buffer instead of resurrecting this abandoned job. */
                slot->cancelled = true;
            }
            portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
            ESP_LOGE(TAG,
                     "H264 overlap unsafe: buffer=%d complete=%d "
                     "frame_start_us=%" PRId64 " eof_us=%" PRId64
                     " encode_done_us=%" PRId64,
                     overlap_buffer_index, overlap_complete,
                     capture_timestamp_us, capture_eof_timestamp_us, end_us);
            video_metrics_record_failed_encode(ctx, encode_time_us, false);
            goto cancel_output;
        }
    }

    uint32_t log_interval = ctx->h264_fps ?
                            (uint32_t)ctx->h264_fps * 2U : 60U;
    if (capture_sequence == 1 ||
        (overlap_buffer_index < 0 &&
         (capture_sequence % log_interval) == 0) ||
        hr != ESP_H264_ERR_OK) {
        if (overlap_buffer_index >= 0) {
            ESP_LOGI(TAG,
                     "H264YUV %s async: capture_seq=%" PRIu32
                     " frame_start_us=%" PRId64 " eof_us=%" PRId64
                     " encode_start_us=%" PRId64 " lead_us=%" PRId64
                     " done_after_eof_us=%" PRId64
                     " ret=%d type=%d len=%" PRIu32 " time_us=%" PRIu32,
                     path_name, capture_sequence, capture_timestamp_us,
                     capture_eof_timestamp_us, start_us,
                     start_us - capture_timestamp_us,
                     end_us - capture_eof_timestamp_us,
                     hr, out_frame.frame_type, out_frame.length,
                     encode_time_us);
        } else {
            ESP_LOGI(TAG,
                     "H264YUV %s async: capture_seq=%" PRIu32
                     " eof_us=%" PRId64 " encode_start_us=%" PRId64
                     " handoff_us=%" PRId64
                     " ret=%d type=%d len=%" PRIu32 " time_us=%" PRIu32,
                     path_name, capture_sequence, capture_timestamp_us,
                     start_us,
                     start_us >= capture_timestamp_us ?
                        start_us - capture_timestamp_us : 0,
                     hr, out_frame.frame_type, out_frame.length,
                     encode_time_us);
        }
    }

    if (hr != ESP_H264_ERR_OK || out_frame.length == 0) {
        video_metrics_record_failed_encode(
            ctx, encode_time_us,
            hr == ESP_H264_ERR_MEM || hr == ESP_H264_ERR_OVERFLOW);
        /* h264_sync_idr_arm() may have moved a generation token to the
         * transport's inflight state.  Retire/requeue it on every failed
         * encode so a transient hardware error cannot deadlock a new peer in
         * waiting-for-sync forever. */
        goto cancel_output;
    }

    bool keyframe = out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR;
    /* Only this task writes encode_frames; the frame is counted below. */
    app_h264_frame_meta_t meta = {
        .sequence = ctx->metrics.encode_frames + 1U,
        .capture_sequence = capture_sequence,
        .capture_timestamp_us = capture_timestamp_us,
        .capture_eof_timestamp_us = capture_eof_timestamp_us,
        .ppa_done_timestamp_us = 0,
        .encode_start_timestamp_us = start_us,
        .encode_done_timestamp_us = end_us,
        .rtp_timestamp_90khz =
            (uint32_t)(((uint64_t)capture_timestamp_us * 9U) / 100U),
        .sync_idr_generation = ctx->h264_sync_idr_generation,
        .width = ctx->h264_width,
        .height = ctx->h264_height,
        .fps = ctx->h264_fps,
        .keyframe = keyframe,
        .encode_overlapped_capture = overlap_buffer_index >= 0,
    };
    bool sink_accepted = webrtc_transport_commit_access_unit(
        &rtc_output, out_frame.length, &meta);
    rtc_reserved = false; /* Commit consumes the lease even when rejected. */
    video_metrics_record_frame(ctx, encode_time_us, capture_sequence,
                               out_frame.length, keyframe, sink_accepted,
                               capture_timestamp_us, start_us);
    if (!sink_accepted) {
        goto cancel_output;
    }
    h264_sync_idr_production_complete(ctx, keyframe && sink_accepted);
    return;

cancel_output:
    /* A successfully encoded but unpublished picture may already be the
     * encoder's reference. Restart the chain before the next published frame
     * so the receiver cannot depend on an unpublished reference picture. */
    {
        esp_h264_enc_param_hw_handle_t param = NULL;
        esp_h264_err_t recovery = esp_h264_enc_hw_get_param_hd(ctx->h264_enc, &param);
        if (recovery == ESP_H264_ERR_OK) {
            recovery = esp_h264_enc_force_idr(&param->base);
        }
        if (recovery != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H264 output cancellation could not restart IDR: %d", recovery);
        }
    }
    if (rtc_reserved) webrtc_transport_cancel_access_unit(&rtc_output);
    h264_sync_idr_production_complete(ctx, false);
}

static void publish_prepared_h264_mode(app_ctx_t *ctx)
{
    if (ctx->preview_only_active || !ctx->h264_enc) {
        return;
    }
    /* The encoder is already materialized. Publish its track before capture
     * can dispatch a frame, rather than opening a placeholder peer and then
     * rebuilding it from reserve_access_unit(). This also avoids reserving a
     * second future encoder block during reconnect admission. */
    esp_err_t ret = webrtc_transport_set_mode(
        ctx->h264_width, ctx->h264_height, ctx->h264_fps);
    ctx->h264_mode_refused = ret == ESP_ERR_NOT_SUPPORTED;
    if (ret != ESP_OK) {
        /* First-frame admission retains its existing validation/retry. */
        ESP_LOGW(TAG, "prepared H264 mode unavailable: %s", esp_err_to_name(ret));
    }
}

static void direct_h264_pipeline_activate(app_ctx_t *ctx)
{
    publish_prepared_h264_mode(ctx);
    portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
    ctx->direct_h264_generation++;
    if (ctx->direct_h264_generation == 0) {
        ctx->direct_h264_generation++;
    }
    ctx->direct_h264_ready_index = DIRECT_H264_INVALID_INDEX;
    ctx->direct_h264_encoding_index = DIRECT_H264_INVALID_INDEX;
    for (size_t i = 0; i < CSI_FRAME_BUFFER_COUNT; i++) {
        ctx->direct_h264_slots[i].generation = ctx->direct_h264_generation;
        ctx->direct_h264_slots[i].capture_sequence = 0;
        ctx->direct_h264_slots[i].csi_dma_start_timestamp_us = 0;
        ctx->direct_h264_slots[i].csi_dma_eof_timestamp_us = 0;
        ctx->direct_h264_slots[i].capture_complete = false;
        ctx->direct_h264_slots[i].overlap_candidate = false;
        ctx->direct_h264_slots[i].cancelled = false;
    }
    portENTER_CRITICAL(&s_csi_buffer_mux);
    uint32_t ready = ctx->ready_buffers & csi_all_buffer_mask(ctx);
    for (uint8_t i = 0; i < ctx->capture_buffer_count; i++) {
        uint32_t bit = 1u << i;
        if (ready & bit) {
            ctx->ready_buffers &= ~bit;
            ctx->free_buffers |= bit;
            ctx->capture_received_sizes[i] = 0;
            ctx->capture_buffer_sequences[i] = 0;
            ctx->capture_buffer_timestamps_us[i] = 0;
            ctx->metrics.capture_stale_drops++;
        }
    }
    ctx->direct_h264_active = true;
    portEXIT_CRITICAL(&s_csi_buffer_mux);
    portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
    ESP_LOGI(TAG, "direct frame pipeline active generation=%" PRIu32
             " mode=%s", ctx->direct_h264_generation,
             ctx->preview_only_active ? "screen-only" : "h264-only");
}

static void direct_h264_pipeline_deactivate(app_ctx_t *ctx)
{
    int8_t ready_index = DIRECT_H264_INVALID_INDEX;
    portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
    ctx->direct_h264_active = false;
    ctx->direct_h264_generation++;
    if (ctx->direct_h264_generation == 0) {
        ctx->direct_h264_generation++;
    }
    if (ctx->direct_h264_ready_index >= 0 &&
        (uint8_t)ctx->direct_h264_ready_index < ctx->capture_buffer_count) {
        ready_index = ctx->direct_h264_ready_index;
    }
    ctx->direct_h264_ready_index = DIRECT_H264_INVALID_INDEX;
    portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);

    /* READY is still represented by its CSI LOCKED bit.  Detach it under the
     * direct-pipeline mux, then return that exact bit without touching a frame
     * that the encoder task may own. */
    csi_release_consumed_buffer(ctx, ready_index);
    if (ctx->direct_h264_encode_task) {
        xTaskNotifyGive(ctx->direct_h264_encode_task);
    }
}

static esp_err_t direct_h264_pipeline_drain(app_ctx_t *ctx, uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    if (ctx->direct_h264_encode_task) {
        xTaskNotifyGive(ctx->direct_h264_encode_task);
    }

    while (true) {
        bool drained;
        portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
        drained = ctx->direct_h264_ready_index == DIRECT_H264_INVALID_INDEX &&
                  ctx->direct_h264_encoding_index == DIRECT_H264_INVALID_INDEX;
        portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
        if (drained) {
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGE(TAG,
                     "direct H264 pipeline drain timeout ready=%d encoding=%d",
                     ctx->direct_h264_ready_index,
                     ctx->direct_h264_encoding_index);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t direct_h264_pipeline_submit(app_ctx_t *ctx,
                                             int8_t capture_buffer_index,
                                             uint32_t capture_sequence,
                                             int64_t capture_timestamp_us)
{
    if (capture_buffer_index < 0 ||
        (uint8_t)capture_buffer_index >= ctx->capture_buffer_count) {
        return ESP_ERR_INVALID_ARG;
    }

    int8_t replaced_index = DIRECT_H264_INVALID_INDEX;
    bool accepted = false;
    portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
    if (ctx->direct_h264_active &&
        ctx->direct_h264_encoding_index != capture_buffer_index) {
        int8_t old_ready = ctx->direct_h264_ready_index;
        if (old_ready >= 0 && old_ready != capture_buffer_index) {
            replaced_index = old_ready;
            ctx->metrics.direct_ready_replaced++;
        }
        direct_h264_slot_t *slot =
            &ctx->direct_h264_slots[(uint8_t)capture_buffer_index];
        slot->generation = ctx->direct_h264_generation;
        slot->capture_sequence = capture_sequence;
        slot->csi_dma_start_timestamp_us = 0;
        slot->csi_dma_eof_timestamp_us = capture_timestamp_us;
        slot->capture_complete = true;
        slot->overlap_candidate = false;
        slot->cancelled = false;
        ctx->direct_h264_ready_index = capture_buffer_index;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);

    /* A replaced frame was never submitted to H.264.  Releasing its one bit
     * makes it immediately eligible for the next CSI LLI replacement. */
    csi_release_consumed_buffer(ctx, replaced_index);
    if (!accepted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->direct_h264_encode_task) {
        xTaskNotifyGive(ctx->direct_h264_encode_task);
    }
    return ESP_OK;
}

static bool IRAM_ATTR direct_h264_pipeline_start_from_isr(
    app_ctx_t *ctx, int8_t capture_buffer_index,
    int64_t csi_dma_start_timestamp_us)
{
    if (capture_buffer_index < 0 ||
        (uint8_t)capture_buffer_index >= ctx->capture_buffer_count) {
        return false;
    }

    BaseType_t high_task_woken = pdFALSE;
    int8_t completed_replaced_index = DIRECT_H264_INVALID_INDEX;
    bool accepted = false;
    portENTER_CRITICAL_ISR(&s_direct_h264_pipeline_mux);
    if (ctx->direct_h264_active && ctx->direct_h264_overlap_enabled &&
        ctx->direct_h264_encoding_index != capture_buffer_index) {
        int8_t old_ready = ctx->direct_h264_ready_index;
        if (old_ready >= 0 && old_ready != capture_buffer_index) {
            direct_h264_slot_t *old_slot =
                &ctx->direct_h264_slots[(uint8_t)old_ready];
            old_slot->cancelled = true;
            if (old_slot->capture_complete) {
                completed_replaced_index = old_ready;
            }
            ctx->metrics.direct_ready_replaced++;
        }

        direct_h264_slot_t *slot =
            &ctx->direct_h264_slots[(uint8_t)capture_buffer_index];
        slot->generation = ctx->direct_h264_generation;
        slot->capture_sequence = 0;
        slot->csi_dma_start_timestamp_us = csi_dma_start_timestamp_us;
        slot->csi_dma_eof_timestamp_us = 0;
        slot->capture_complete = false;
        slot->overlap_candidate = true;
        slot->cancelled = false;
        ctx->direct_h264_ready_index = capture_buffer_index;
        accepted = true;
    }
    portEXIT_CRITICAL_ISR(&s_direct_h264_pipeline_mux);

    csi_release_consumed_buffer_from_isr(ctx, completed_replaced_index);
    if (accepted && ctx->direct_h264_encode_task) {
        vTaskNotifyGiveFromISR(ctx->direct_h264_encode_task,
                               &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

/* Caller holds s_direct_h264_pipeline_mux so the EOF ownership transition is
 * atomic with direct-pipeline activation and teardown. */
static bool IRAM_ATTR direct_h264_pipeline_submit_locked_from_isr(
    app_ctx_t *ctx, int8_t capture_buffer_index, uint32_t capture_sequence,
    int64_t csi_dma_eof_timestamp_us, BaseType_t *high_task_woken)
{
    if (capture_buffer_index < 0 ||
        (uint8_t)capture_buffer_index >= ctx->capture_buffer_count) {
        return false;
    }

    int8_t replaced_index = DIRECT_H264_INVALID_INDEX;
    bool accepted = false;
    direct_h264_slot_t *slot =
        &ctx->direct_h264_slots[(uint8_t)capture_buffer_index];
    if (ctx->direct_h264_active && ctx->direct_h264_overlap_enabled) {
        if (slot->generation == ctx->direct_h264_generation &&
            slot->cancelled) {
            return false;
        }
        if (slot->generation == ctx->direct_h264_generation &&
            slot->overlap_candidate &&
            slot->csi_dma_start_timestamp_us > 0) {
            slot->capture_sequence = capture_sequence;
            slot->csi_dma_eof_timestamp_us = csi_dma_eof_timestamp_us;
            slot->capture_complete = true;
            accepted = ctx->direct_h264_encoding_index ==
                           capture_buffer_index ||
                       ctx->direct_h264_ready_index ==
                           capture_buffer_index;
            if (accepted && ctx->direct_h264_encode_task) {
                vTaskNotifyGiveFromISR(ctx->direct_h264_encode_task,
                                       high_task_woken);
            }
            return accepted;
        }
        /* The start callback for the next frame runs before this EOF callback.
         * During overlap activation, discard an older legacy completion rather
         * than replace that newer in-flight frame. */
        if (ctx->direct_h264_ready_index >= 0) {
            const direct_h264_slot_t *ready =
                &ctx->direct_h264_slots[(uint8_t)ctx->direct_h264_ready_index];
            if (ready->generation == ctx->direct_h264_generation &&
                ready->overlap_candidate) {
                return false;
            }
        }
    }
    if (ctx->direct_h264_active &&
        ctx->direct_h264_encoding_index != capture_buffer_index) {
        int8_t old_ready = ctx->direct_h264_ready_index;
        if (old_ready >= 0 && old_ready != capture_buffer_index) {
            replaced_index = old_ready;
            ctx->metrics.direct_ready_replaced++;
        }
        slot->generation = ctx->direct_h264_generation;
        slot->capture_sequence = capture_sequence;
        slot->csi_dma_start_timestamp_us = 0;
        slot->csi_dma_eof_timestamp_us = csi_dma_eof_timestamp_us;
        slot->capture_complete = true;
        slot->overlap_candidate = false;
        slot->cancelled = false;
        ctx->direct_h264_ready_index = capture_buffer_index;
        accepted = true;
    }

    csi_release_consumed_buffer_from_isr(ctx, replaced_index);
    if (accepted && ctx->direct_h264_encode_task) {
        vTaskNotifyGiveFromISR(ctx->direct_h264_encode_task,
                               high_task_woken);
    }
    return accepted;
}

static bool screen_preview_pipeline_submit(app_ctx_t *ctx,
                                           int8_t capture_buffer_index,
                                           uint32_t capture_sequence,
                                           size_t raw_size)
{
    if (ctx->screen_preview_queue == NULL || capture_buffer_index < 0 ||
        (uint8_t)capture_buffer_index >= ctx->capture_buffer_count) {
        return false;
    }
    int64_t now_us = esp_timer_get_time();
#if SCREEN_PREVIEW_INTERVAL_US > 0
    if (ctx->screen_preview_last_enqueue_us != 0 &&
        now_us - ctx->screen_preview_last_enqueue_us <
            SCREEN_PREVIEW_INTERVAL_US) {
        return false;
    }
#endif

    screen_preview_job_t job = {
        .capture_buffer_index = capture_buffer_index,
        .capture_sequence = capture_sequence,
        .raw_size = raw_size,
        .width = ctx->capture_width,
        .height = ctx->capture_height,
        .fps = ctx->active_timings.fps != 0U ?
               ctx->active_timings.fps : ctx->h264_fps,
    };
    __atomic_fetch_add(&ctx->screen_preview_owned_jobs, 1U,
                       __ATOMIC_ACQ_REL);
    if (xQueueSend(ctx->screen_preview_queue, &job, 0) != pdTRUE) {
        __atomic_fetch_sub(&ctx->screen_preview_owned_jobs, 1U,
                           __ATOMIC_ACQ_REL);
        return false;
    }
    ctx->screen_preview_last_enqueue_us = now_us;
    return true;
}

static void screen_preview_task(void *arg)
{
    app_ctx_t *ctx = (app_ctx_t *)arg;
    screen_preview_job_t job;
    for (;;) {
        if (xQueueReceive(ctx->screen_preview_queue, &job,
                          portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const uint8_t *pixels = (const uint8_t *)ctx->capture_buffers[
            (uint8_t)job.capture_buffer_index];
        esp_err_t error = TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420 ?
            jetkvm_screen_preview_submit_ouyy_evyy(
                pixels, job.raw_size, job.width, job.height, job.fps) :
            jetkvm_screen_preview_submit_uyvy(
                pixels, job.raw_size, job.width, job.height, job.fps);
        if (error == ESP_OK && !ctx->screen_preview_logged) {
            ctx->screen_preview_logged = true;
            ESP_LOGI(TAG,
                     "LCD HDMI preview active %ux%u@%u (decoupled PPA)",
                     job.width, job.height, job.fps);
        }
        csi_release_consumed_buffer(ctx, job.capture_buffer_index);
        __atomic_fetch_sub(&ctx->screen_preview_owned_jobs, 1U,
                           __ATOMIC_ACQ_REL);
    }
}

#if !CONFIG_JETKVM_SCREEN_BUTTONS
static void video_mode_button_task(void *arg)
{
    app_ctx_t *ctx = (app_ctx_t *)arg;
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << VIDEO_MODE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* SW3 already has an external 10 kOhm pull-up to ESP_3V3. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    bool stable_pressed = gpio_get_level(VIDEO_MODE_BUTTON_GPIO) == 0;
    bool sampled_pressed = stable_pressed;
    uint8_t stable_samples = 0;
    for (;;) {
        bool pressed = gpio_get_level(VIDEO_MODE_BUTTON_GPIO) == 0;
        if (pressed == sampled_pressed) {
            if (stable_samples < VIDEO_MODE_BUTTON_DEBOUNCE_SAMPLES) {
                stable_samples++;
            }
        } else {
            sampled_pressed = pressed;
            stable_samples = 1;
        }
        if (stable_samples >= VIDEO_MODE_BUTTON_DEBOUNCE_SAMPLES &&
            stable_pressed != sampled_pressed) {
            stable_pressed = sampled_pressed;
            if (stable_pressed) {
                bool next_preview = !__atomic_load_n(
                    &ctx->preview_only_requested, __ATOMIC_ACQUIRE);
                __atomic_store_n(&ctx->preview_only_requested, next_preview,
                                 __ATOMIC_RELEASE);
                ESP_LOGI(TAG, "SW3 requested video mode=%s",
                         next_preview ? "screen-only" : "h264-only");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(VIDEO_MODE_BUTTON_POLL_MS));
    }
}
#endif /* !CONFIG_JETKVM_SCREEN_BUTTONS */

static esp_err_t screen_preview_pipeline_drain(app_ctx_t *ctx,
                                               uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() +
                          (int64_t)timeout_ms * 1000;
    while (__atomic_load_n(&ctx->screen_preview_owned_jobs,
                           __ATOMIC_ACQUIRE) != 0U) {
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGE(TAG, "LCD preview drain timeout owned=%" PRIu32,
                     __atomic_load_n(&ctx->screen_preview_owned_jobs,
                                     __ATOMIC_RELAXED));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

static void direct_h264_pipeline_encode_task(void *arg)
{
    app_ctx_t *ctx = (app_ctx_t *)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (true) {
            int8_t buffer_index = DIRECT_H264_INVALID_INDEX;
            direct_h264_slot_t job = {0};
            portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
            if (ctx->direct_h264_active &&
                ctx->direct_h264_encoding_index == DIRECT_H264_INVALID_INDEX &&
                ctx->direct_h264_ready_index >= 0 &&
                (uint8_t)ctx->direct_h264_ready_index < ctx->capture_buffer_count) {
                int8_t ready_index = ctx->direct_h264_ready_index;
                direct_h264_slot_t *ready =
                    &ctx->direct_h264_slots[(uint8_t)ready_index];
                if (ready->generation == ctx->direct_h264_generation) {
                    ctx->direct_h264_ready_index = DIRECT_H264_INVALID_INDEX;
                    ctx->direct_h264_encoding_index = ready_index;
                    buffer_index = ready_index;
                    job = *ready;
                }
            }
            portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);

            if (buffer_index < 0) {
                break;
            }
            if (ctx->preview_only_active) {
                size_t raw_size = h264_raw_frame_size(
                    ctx->capture_width, ctx->capture_height,
                    TC_H264_RAW_FORMAT);
                portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
                if (ctx->direct_h264_encoding_index == buffer_index) {
                    ctx->direct_h264_encoding_index =
                        DIRECT_H264_INVALID_INDEX;
                }
                bool another_ready = ctx->direct_h264_active &&
                                     ctx->direct_h264_ready_index >= 0;
                portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
                bool preview_owned = job.capture_complete && !job.cancelled &&
                    screen_preview_pipeline_submit(ctx, buffer_index,
                                                   job.capture_sequence,
                                                   raw_size);
                if (!preview_owned) {
                    csi_release_consumed_buffer(ctx, buffer_index);
                }
                if (!another_ready) {
                    break;
                }
                continue;
            }
            bool overlap_job = job.overlap_candidate &&
                               job.csi_dma_start_timestamp_us > 0 &&
                               !job.cancelled;
            if (overlap_job && !job.capture_complete) {
                int64_t start_deadline_us =
                    job.csi_dma_start_timestamp_us +
                    DIRECT_H264_OVERLAP_LEAD_US;
                while (true) {
                    int64_t remaining_us =
                        start_deadline_us - esp_timer_get_time();
                    if (remaining_us <= 0) {
                        break;
                    }
                    if (remaining_us > 1500) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                    } else {
                        esp_rom_delay_us((uint32_t)remaining_us);
                    }
                }
            }
            int64_t encode_dispatch_us = esp_timer_get_time();
            if (overlap_job && !job.capture_complete &&
                encode_dispatch_us - job.csi_dma_start_timestamp_us >
                    DIRECT_H264_OVERLAP_MAX_DISPATCH_US) {
                /* The encoder's per-frame service time is only just below a
                 * 30 Hz period.  Copy/transport work after frame-done can
                 * therefore let its dispatch phase drift later by a few
                 * microseconds per frame until the access unit is nearly one
                 * refresh old.  Drop this in-flight candidate and start the
                 * next frame at the configured producer lead instead of
                 * preserving that accumulated latency. */
                bool release_completed = false;
                portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
                direct_h264_slot_t *slot =
                    &ctx->direct_h264_slots[(uint8_t)buffer_index];
                if (slot->generation == job.generation &&
                    ctx->direct_h264_encoding_index == buffer_index) {
                    slot->cancelled = true;
                    release_completed = slot->capture_complete;
                    ctx->direct_h264_encoding_index =
                        DIRECT_H264_INVALID_INDEX;
                    ctx->metrics.direct_expired_drops++;
                }
                bool another_ready = ctx->direct_h264_active &&
                                     ctx->direct_h264_ready_index >= 0;
                portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);

                /* If EOF won the mux race, its callback left the accepted
                 * buffer LOCKED for this task.  Otherwise the callback sees
                 * cancelled and releases it when DMA really completes. */
                if (release_completed) {
                    csi_release_consumed_buffer(ctx, buffer_index);
                }
                if (!another_ready) {
                    break;
                }
                continue;
            }
            uint64_t ready_age_us =
                job.capture_complete &&
                encode_dispatch_us >= job.csi_dma_eof_timestamp_us ?
                (uint64_t)(encode_dispatch_us -
                           job.csi_dma_eof_timestamp_us) : 0;
            /* Keep only the newest completed frame, with at most one frame
             * period of queueing. A 1 ms cutoff drops good captures whenever
             * encoding plus AU submission slightly exceeds the input period. */
            uint32_t max_ready_age_us = 1000000U /
                (ctx->h264_fps ? ctx->h264_fps : 30U);
            if (job.capture_complete && ready_age_us > max_ready_age_us) {
                ctx->metrics.direct_expired_drops++;
                csi_release_consumed_buffer(ctx, buffer_index);
                portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
                if (ctx->direct_h264_encoding_index == buffer_index) {
                    ctx->direct_h264_encoding_index =
                        DIRECT_H264_INVALID_INDEX;
                }
                bool another_ready = ctx->direct_h264_active &&
                                     ctx->direct_h264_ready_index >= 0;
                portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
                if (ctx->metrics.direct_expired_drops == 1 ||
                    (ctx->metrics.direct_expired_drops % 300) == 0) {
                    ESP_LOGI(TAG,
                             "direct cadence skip capture_seq=%" PRIu32
                             " age_us=%" PRIu64 " skips=%" PRIu32,
                             job.capture_sequence, ready_age_us,
                             ctx->metrics.direct_expired_drops);
                }
                if (!another_ready) {
                    break;
                }
                continue;
            }
            size_t raw_size = h264_raw_frame_size(ctx->h264_width,
                                                  ctx->h264_height,
                                                  ctx->h264_pic_type);
            h264_pipeline_encode_one(
                ctx, (uint8_t *)ctx->capture_buffers[(uint8_t)buffer_index],
                raw_size, job.capture_sequence,
                overlap_job ? job.csi_dma_start_timestamp_us :
                              job.csi_dma_eof_timestamp_us,
                overlap_job ? "direct-overlap" : "direct",
                overlap_job ? buffer_index : DIRECT_H264_INVALID_INDEX,
                job.generation);

            /* H.264 mode is deliberately exclusive: once the synchronous
             * encoder is done, return the source directly to CSI. Never hand
             * an encoded source to the PPA preview task. */
            portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
            if (ctx->direct_h264_encoding_index == buffer_index) {
                ctx->direct_h264_encoding_index = DIRECT_H264_INVALID_INDEX;
            }
            bool another_ready = ctx->direct_h264_active &&
                                     ctx->direct_h264_ready_index >= 0;
            portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
            csi_release_consumed_buffer(ctx, buffer_index);
            if (!another_ready) {
                break;
            }
        }
    }
}

static bool timings_equal(const tc358743_timings_t *a, const tc358743_timings_t *b)
{
    return a->width == b->width &&
           a->height == b->height &&
           a->frame_width == b->frame_width &&
           a->frame_height == b->frame_height &&
           abs((int)a->fps - (int)b->fps) <= 1 &&
           a->interlaced == b->interlaced;
}

static esp_err_t csi_destroy(app_ctx_t *ctx)
{
    esp_err_t first_error = ESP_OK;
    direct_h264_pipeline_deactivate(ctx);
    /* Stop the producer before waiting for the synchronous H.264 consumer. A
     * CSI buffer remains LOCKED while direct H.264 owns it. */
    if (ctx->cam) {
        esp_err_t ret = esp_cam_ctlr_stop(ctx->cam);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            /* A failed stop can leave DMA or its completion callback owning
             * the ring and capture buffers. Preserve them for the caller's
             * fatal recovery; continuing teardown would free live memory. */
            ESP_LOGE(TAG, "esp_cam_ctlr_stop failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    esp_err_t drain_ret = direct_h264_pipeline_drain(ctx, DIRECT_H264_DRAIN_MS);
    if (drain_ret != ESP_OK) {
        /* The encoder task can still own a raw frame and its output lease.
         * Preserve both until fatal recovery or a later successful drain. */
        return drain_ret;
    }
    esp_err_t preview_drain_ret = screen_preview_pipeline_drain(
        ctx, SCREEN_PREVIEW_DRAIN_MS);
    if (preview_drain_ret != ESP_OK) {
        /* A PPA DMA still owns a capture buffer. Never free that memory out
         * from under hardware; the owner will reset on this fatal teardown. */
        (void)jetkvm_screen_preview_stop();
        return preview_drain_ret;
    }
    (void)jetkvm_screen_preview_stop();
    ctx->direct_h264_overlap_enabled = false;

    /* Stop the transport from sending this mode. The WebRTC peer stays;
     * tc_try_candidates() releases it only when the rebuild runs out of
     * memory. */
    esp_err_t transport_ret = webrtc_transport_clear_mode_and_wait(3000);
    if (transport_ret != ESP_OK) {
        ESP_LOGE(TAG, "WebRTC mode teardown failed: %s", esp_err_to_name(transport_ret));
    }
    /* The capture slots and the DB buffer are the video pool and outlive this
     * teardown; video_mode_prepare() releases only an encoder of another mode. */
    h264_sync_idr_production_complete(ctx, false);
    if (ctx->cam) {
        esp_err_t ret;
        ret = esp_cam_ctlr_disable(ctx->cam);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_cam_ctlr_disable failed: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
        /* A DMA drain timeout preserves the handle and all hardware-owned
         * memory. Once drained, the deleter consumes the handle even if a
         * secondary resource cleanup reports an error. */
        ret = esp_cam_ctlr_del(ctx->cam);
        if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NOT_ALLOWED) {
            ESP_LOGE(TAG, "CSI DMA delete could not quiesce; preserving buffers");
            return ret;
        }
        ctx->cam = NULL;
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_cam_ctlr_del failed: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }
    if (ctx->bridge.isp) {
        esp_err_t ret = esp_isp_disable(ctx->bridge.isp);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_isp_disable failed: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
        ret = esp_isp_del_processor(ctx->bridge.isp);
        if (ret == ESP_OK) {
            ctx->bridge.isp = NULL;
        } else {
            ESP_LOGE(TAG, "esp_isp_del_processor failed: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }
    for (size_t i = 0; i < CSI_FRAME_BUFFER_COUNT; i++) {
        if (ctx->capture_cache_fastpaths[i] !=
            DMA_CACHE_FASTPATH_INVALID_HANDLE) {
            esp_err_t cache_ret = dma_cache_fastpath_unregister(
                ctx->capture_cache_fastpaths[i]);
            if (cache_ret != ESP_OK && first_error == ESP_OK) {
                first_error = cache_ret;
            }
            ctx->capture_cache_fastpaths[i] =
                DMA_CACHE_FASTPATH_INVALID_HANDLE;
        }
    }
    ctx->frame_buffer = NULL;
    ctx->frame_buffer_size = 0;
    ctx->capture_buffer_capacity = 0;
    ctx->capture_buffer_count = 0;
    ctx->capture_next_buffer = 0;
    ctx->free_buffers = 0;
    ctx->inflight_buffers = 0;
    ctx->ready_buffers = 0;
    ctx->locked_buffers = 0;
    ctx->csi_no_free_buffers = 0;
    ctx->current_capture_sequence = 0;
    ctx->current_capture_timestamp_us = 0;
    memset((void *)ctx->capture_buffer_sequences, 0, sizeof(ctx->capture_buffer_sequences));
    memset((void *)ctx->capture_buffer_timestamps_us, 0, sizeof(ctx->capture_buffer_timestamps_us));
    ctx->frame_buffer_owned_by_driver = false;
    ctx->capture_width = 0;
    ctx->capture_height = 0;
    ctx->frames_consumed = 0;
    ctx->consecutive_timeouts = 0;
    memset(&ctx->active_timings, 0, sizeof(ctx->active_timings));
    ctx->active_preset = NULL;
    ctx->bridge.active_candidate = NULL;
    ctx->video_sleep_resume_direct_pipeline = false;
    ctx->h264_mode_refused = false;
    ctx->screen_preview_logged = false;
    ctx->screen_preview_last_enqueue_us = 0;
    return first_error != ESP_OK ? first_error : transport_ret;
}

/* The receiver reports a mode and video does not come (hdmi_health.h): the
 * bridge starts over as at boot, from its reset pin to the applied EDID
 * preset, whose write holds HPD low for TC_HPD_LOW_MIN_MS. The caller has
 * torn capture down. */
static esp_err_t hdmi_bridge_reset(app_ctx_t *ctx)
{
    tc_hw_reset(&ctx->bridge);
    ESP_RETURN_ON_ERROR(tc_probe_chip(&ctx->bridge), TAG,
                        "bridge probe after reset");
    return tc_initial_setup(
        &ctx->bridge, TC358743_REFCLK_HZ,
        p4_edid_descriptor(p4_edid_control_applied_preset()));
}

/* OS Services guest display (jetkvm_video_present_guest). Any task queues
 * a request under s_guest_display_mux; only the capture task writes the
 * bridge. The newest request wins, and `generation` keeps an older
 * request's outcome from overwriting the flags a newer one has set.
 * Physical capture, and with it the transport mode (csi_destroy), is torn
 * down before a guest EDID is written, and `presented` is set only after
 * HPD is high again, so a guest stream started on `presented` is never
 * cleared from here. */
typedef enum {
    GUEST_DISPLAY_REQUEST_NONE = 0,
    GUEST_DISPLAY_REQUEST_PRESENT,
    GUEST_DISPLAY_REQUEST_RESTORE,
} guest_display_request_t;

/* What the bridge holds; capture task only. RESTORING: the preset still
 * has to go back (after a restore request or a failed write). */
typedef enum {
    GUEST_DISPLAY_BRIDGE_PHYSICAL = 0,
    GUEST_DISPLAY_BRIDGE_GUEST,
    GUEST_DISPLAY_BRIDGE_RESTORING,
} guest_display_bridge_t;

static portMUX_TYPE s_guest_display_mux = portMUX_INITIALIZER_UNLOCKED;
/* Only tasks touch these, so they live in PSRAM (sdkconfig.defaults). */
static EXT_RAM_BSS_ATTR struct {
    uint8_t edid[256];
    guest_display_request_t request;
    uint32_t generation;
    bool owns;
    bool presented;
} s_guest_display;
static EXT_RAM_BSS_ATTR struct {
    uint8_t edid[256];
    guest_display_bridge_t bridge;
} s_guest_display_owner;
/* Capture buffers lent to service video (jetkvm_video_lend_guest_buffers());
 * set under s_guest_display_mux. */
static bool s_guest_buffers_lent;

static void guest_display_queue(guest_display_request_t request,
                                const uint8_t *edid)
{
    portENTER_CRITICAL(&s_guest_display_mux);
    if (edid) {
        memcpy(s_guest_display.edid, edid, sizeof(s_guest_display.edid));
        __atomic_store_n(&s_guest_display.owns, true, __ATOMIC_RELEASE);
    }
    s_guest_display.request = request;
    s_guest_display.generation++;
    __atomic_store_n(&s_guest_display.presented, false, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_guest_display_mux);
}

static guest_display_request_t guest_display_take(uint32_t *generation)
{
    portENTER_CRITICAL(&s_guest_display_mux);
    const guest_display_request_t request = s_guest_display.request;
    s_guest_display.request = GUEST_DISPLAY_REQUEST_NONE;
    *generation = s_guest_display.generation;
    if (request == GUEST_DISPLAY_REQUEST_PRESENT) {
        memcpy(s_guest_display_owner.edid, s_guest_display.edid,
               sizeof(s_guest_display_owner.edid));
    }
    portEXIT_CRITICAL(&s_guest_display_mux);
    return request;
}

static void guest_display_publish(uint32_t generation, bool owns,
                                  bool presented)
{
    portENTER_CRITICAL(&s_guest_display_mux);
    if (s_guest_display.generation == generation) {
        __atomic_store_n(&s_guest_display.owns, owns, __ATOMIC_RELEASE);
        __atomic_store_n(&s_guest_display.presented, presented,
                         __ATOMIC_RELEASE);
    }
    portEXIT_CRITICAL(&s_guest_display_mux);
}

/* Writes back the preset the bridge had before the handover; a setEDID
 * queued meanwhile stays pending and applies on a later loop pass. */
static void guest_display_restore(app_ctx_t *ctx, uint32_t generation)
{
    s_guest_display_owner.bridge = GUEST_DISPLAY_BRIDGE_RESTORING;
    esp_err_t error = tc_program_edid(
        &ctx->bridge, p4_edid_descriptor(p4_edid_control_applied_preset()));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "EDID preset restore failed: %s; retrying",
                 esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    s_guest_display_owner.bridge = GUEST_DISPLAY_BRIDGE_PHYSICAL;
    guest_display_publish(generation, false, false);
    ESP_LOGI(TAG, "physical display restored; capture resumes");
}

static void guest_display_present(app_ctx_t *ctx, uint32_t generation)
{
    if (s_guest_display_owner.bridge == GUEST_DISPLAY_BRIDGE_PHYSICAL) {
        (void)tc_enable_stream(&ctx->bridge, false);
        esp_err_t error = csi_destroy(ctx);
        if (error != ESP_OK) {
            /* The preset is still on the bridge. Ending ownership fails the
             * handover, and the capture loop rebuilds as after a failed
             * host display sync. */
            ESP_LOGE(TAG, "guest display capture teardown failed: %s",
                     esp_err_to_name(error));
            guest_display_publish(generation, false, false);
            return;
        }
    }
    esp_err_t error = tc_program_edid_bytes(&ctx->bridge,
                                            s_guest_display_owner.edid);
    if (error != ESP_OK) {
        /* Part of the guest EDID may be on the bridge. Ending ownership
         * with the preset back tells OS Services the handover failed. */
        ESP_LOGE(TAG, "guest display EDID failed: %s",
                 esp_err_to_name(error));
        guest_display_restore(ctx, generation);
        return;
    }
    s_guest_display_owner.bridge = GUEST_DISPLAY_BRIDGE_GUEST;
    guest_display_publish(generation, true, true);
    ESP_LOGI(TAG, "guest display presented; physical capture stopped");
}

/* Applies the newest request (capture task only, 2 s per EDID written).
 * Returns true while the guest display holds the receiver, or an EDID was
 * just written. */
static bool guest_display_serve(app_ctx_t *ctx)
{
    const guest_display_bridge_t bridge = s_guest_display_owner.bridge;
    /* During an OTA install requests wait, as a queued EDID preset does. */
    if (__atomic_load_n(&s_capture_suspended, __ATOMIC_ACQUIRE)) {
        return bridge != GUEST_DISPLAY_BRIDGE_PHYSICAL;
    }
    uint32_t generation;
    const guest_display_request_t request = guest_display_take(&generation);
    if (request == GUEST_DISPLAY_REQUEST_PRESENT) {
        guest_display_present(ctx, generation);
        return true;
    }
    if (bridge == GUEST_DISPLAY_BRIDGE_PHYSICAL) {
        if (request == GUEST_DISPLAY_REQUEST_RESTORE) {
            /* Nothing to write: no present ran since the last restore. */
            guest_display_publish(generation, false, false);
        }
        return false;
    }
    if (request == GUEST_DISPLAY_REQUEST_RESTORE ||
        bridge == GUEST_DISPLAY_BRIDGE_RESTORING) {
        guest_display_restore(ctx, generation);
    }
    return true;
}

/* True when the physical capture loop must not run this pass: the guest
 * display holds the receiver or service video holds capture buffers. */
static bool guest_display_service(app_ctx_t *ctx)
{
    const bool held = guest_display_serve(ctx);
    return held || __atomic_load_n(&s_guest_buffers_lent, __ATOMIC_ACQUIRE);
}

static bool p4_host_display_should_advertise(void)
{
    static int64_t idle_since_us;
    /* The guest display stays visible whatever the idle policy; the grace
     * below starts over after the restore. */
    if (jetkvm_video_guest_owns_display()) {
        idle_since_us = 0;
        return true;
    }
    if (!__atomic_load_n(&s_host_display_idle_enabled, __ATOMIC_ACQUIRE)) {
        return true;
    }
    webrtc_transport_stats_t stats = {0};
    if (webrtc_transport_get_stats(&stats) != ESP_OK) {
        /* Fail open: a diagnostics failure must never strand the host with
         * HPD low or make the setting impossible to turn back off. */
        return true;
    }
    /* Session creation precedes peer/video negotiation. Raising HPD on this
     * edge lets a previously hidden source establish a mode before the peer
     * needs its first decodable access unit. */
    if (stats.browser_session_active) {
        idle_since_us = 0;
        return true;
    }
    /* When a rebuild has to release the peer for memory, the signaling
     * session closes and the browser re-offers about a second later. Hiding
     * the display inside that gap drops the source's mode, which forces
     * another rebuild: an endless loop. Upstream hides on the session count
     * alone and never closes its peer for video, so a short grace keeps the
     * observable behaviour equal. */
    const int64_t now_us = esp_timer_get_time();
    if (idle_since_us == 0) {
        idle_since_us = now_us;
    }
    return now_us - idle_since_us < HOST_DISPLAY_IDLE_GRACE_US;
}

/* When the host display policy last lowered HPD. */
static int64_t s_host_display_hidden_since_us;

/* Time HPD has been low since the policy lowered it, in microseconds. */
static int64_t p4_host_display_hidden_us(void)
{
    return esp_timer_get_time() - s_host_display_hidden_since_us;
}

/* True once HPD may rise again: it has been low for TC_HPD_LOW_MIN_MS. The
 * video task calls this every pass while HPD is low, so a raise held back
 * here happens on the first pass after the minimum. */
static bool p4_host_display_may_raise(void)
{
    return p4_host_display_hidden_us() >= (int64_t)TC_HPD_LOW_MIN_MS * 1000;
}

static esp_err_t p4_host_display_sync(app_ctx_t *ctx, bool *advertised)
{
    ESP_RETURN_ON_FALSE(ctx && advertised, ESP_ERR_INVALID_ARG, TAG,
                        "invalid host display state");
    const bool desired = p4_host_display_should_advertise();
    if (desired == *advertised) {
        return ESP_OK;
    }

    const int64_t hidden_us = desired ? p4_host_display_hidden_us() : 0;
    if (desired && !p4_host_display_may_raise()) {
        return ESP_OK;
    }
    if (!desired) {
        /* Only the HDMI owner reaches this path. The session is already gone,
         * so draining the old mode cannot deadlock on its RPC peer. */
        ESP_RETURN_ON_ERROR(tc_enable_stream(&ctx->bridge, false), TAG,
                            "mute stream before hiding host display");
        ESP_RETURN_ON_ERROR(csi_destroy(ctx), TAG,
                            "tear down capture before hiding host display");
        /* The drain above waits for the transport; a browser can open a
         * session (or a rebuild can announce its re-offer) meanwhile. A
         * decision taken before the drain must not lower HPD after it. */
        if (p4_host_display_should_advertise()) {
            ESP_LOGI(TAG, "host display hide cancelled: session active after drain");
            return ESP_OK;
        }
    }
    esp_err_t error = tc_set_hpd_output(&ctx->bridge, desired);
    if (error != ESP_OK) {
        if (!desired) {
            (void)tc_set_hpd_output(&ctx->bridge, true);
        }
        return error;
    }
    *advertised = desired;
    const int idle_policy = __atomic_load_n(&s_host_display_idle_enabled,
                                            __ATOMIC_ACQUIRE);
    if (desired) {
        ESP_LOGI(TAG, "host display advertisement visible idle_policy=%d "
                 "hidden_ms=%" PRId64, idle_policy, hidden_us / 1000);
    } else {
        s_host_display_hidden_since_us = esp_timer_get_time();
        ESP_LOGI(TAG, "host display advertisement hidden idle_policy=%d",
                 idle_policy);
    }
    return ESP_OK;
}

static esp_err_t p4_video_sleep_sync(app_ctx_t *ctx,
                                      int64_t *idle_since_us)
{
    ESP_RETURN_ON_FALSE(ctx && idle_since_us, ESP_ERR_INVALID_ARG, TAG,
                        "invalid video sleep state");
    const int32_t configured = __atomic_load_n(
        &s_video_sleep_after_seconds, __ATOMIC_ACQUIRE);
    webrtc_transport_stats_t stats = {0};
    const bool stats_available = webrtc_transport_get_stats(&stats) == ESP_OK;
    const bool session_active = !stats_available || stats.browser_session_active;

    if (configured < 0 || session_active) {
        *idle_since_us = 0;
        if (__atomic_load_n(&s_video_sleep_active, __ATOMIC_ACQUIRE)) {
            ESP_RETURN_ON_ERROR(p4_video_sleep_resume_capture(ctx), TAG,
                                "resume capture after video sleep failed");
            __atomic_store_n(&s_video_sleep_active, false,
                             __ATOMIC_RELEASE);
            ESP_LOGI(TAG,
                     "HDMI video sleep exited reason=%s HPD remains advertised",
                     configured < 0 ? "disabled" : "browser-session");
        }
        return ESP_OK;
    }

    const int32_t duration_seconds = configured == 0 ?
        VIDEO_SLEEP_UPSTREAM_DEFAULT_SECONDS : configured;
    const int64_t now_us = esp_timer_get_time();
    if (*idle_since_us == 0) {
        *idle_since_us = now_us;
        return ESP_OK;
    }
    if (now_us - *idle_since_us <
        (int64_t)duration_seconds * 1000000LL) {
        return ESP_OK;
    }
    if (__atomic_load_n(&s_video_sleep_active, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }

    /* Power saving is intentionally distinct from hiding the host display:
     * verify HPD high and pause the expensive CSI/H.264 producer while
     * retaining its large buffers. Reallocating those buffers on every wake
     * fragments PSRAM and can eventually prevent a WebRTC peer reservation. */
    ESP_RETURN_ON_ERROR(tc_set_hpd_output(&ctx->bridge, true), TAG,
                        "video sleep HPD fail-open check failed");
    ESP_RETURN_ON_ERROR(tc_enable_stream(&ctx->bridge, false), TAG,
                        "mute stream before video sleep");
    esp_err_t pause_ret = ESP_OK;
    bool direct_pipeline_active;
    portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
    direct_pipeline_active = ctx->direct_h264_active;
    portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
    ctx->video_sleep_resume_direct_pipeline = direct_pipeline_active;
    direct_h264_pipeline_deactivate(ctx);
    if (ctx->cam) {
        esp_err_t stop_ret = esp_cam_ctlr_stop(ctx->cam);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
            pause_ret = stop_ret;
            goto pause_failed;
        }
    }
    pause_ret = direct_h264_pipeline_drain(ctx, DIRECT_H264_DRAIN_MS);
    if (pause_ret != ESP_OK) {
        goto pause_failed;
    }
    /* Clear the advertised mode only after the producer and encoder are
     * fully drained. Otherwise one final access unit can publish the old mode
     * again after the device has entered sleep. */
    pause_ret = webrtc_transport_clear_mode_and_wait(3000);
    if (pause_ret != ESP_OK) {
        goto pause_failed;
    }
    __atomic_store_n(&s_video_sleep_active, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG,
             "HDMI video sleep entered duration=%" PRId32
             "s HPD remains advertised",
             duration_seconds);
    return ESP_OK;

pause_failed:
    ESP_LOGE(TAG, "video sleep pause failed: %s; resuming capture",
             esp_err_to_name(pause_ret));
    esp_err_t resume_ret = p4_video_sleep_resume_capture(ctx);
    if (resume_ret != ESP_OK) {
        ESP_LOGE(TAG, "video sleep pause rollback failed: %s",
                 esp_err_to_name(resume_ret));
    }
    return pause_ret;
}

/* The bridge is the DMA flow controller: it ends a DMA block at the frame end,
 * or after dmablk_size bursts when it misses the frame end, as happens when the
 * TC358743 stream starts or unmutes while the DMA is already armed. The
 * register maximum is 8191 x 512 B = 4.19 MB, more than a 3.11 MB capture
 * buffer, and the overrun lands in the heap block after it. Bound a block to
 * the frame rounded up to whole bursts, so a normal frame still ends before
 * the bound. Every DMA destination holds at least that much: our capture
 * buffers (CSI_CAPTURE_CAPACITY) and the driver's backup buffer
 * (P4_CSI_BACKUP_BUFFER_ALIGN_BYTES). */
#if CSI_DMA_BURST_LEN_OVERRIDE > 0
_Static_assert(P4_CSI_BACKUP_BUFFER_ALIGN_BYTES % CSI_DMA_BURST_BYTES == 0,
               "the CSI backup buffer must hold whole DMA bursts");
static uint32_t csi_dma_block_bursts(const app_ctx_t *ctx)
{
    size_t bursts = CSI_CAPTURE_CAPACITY(ctx->frame_buffer_size) /
                    CSI_DMA_BURST_BYTES;
    if (!ctx->use_driver_backup &&
        bursts > ctx->capture_buffer_capacity / CSI_DMA_BURST_BYTES) {
        bursts = ctx->capture_buffer_capacity / CSI_DMA_BURST_BYTES;
    }
    if (bursts == 0U) {
        return 1U;
    }
    return bursts > CSI_DMA_BLOCK_BURSTS_MAX ?
        CSI_DMA_BLOCK_BURSTS_MAX : (uint32_t)bursts;
}
#endif

static esp_err_t csi_restart_rx(app_ctx_t *ctx)
{
    ESP_RETURN_ON_FALSE(ctx->cam, ESP_ERR_INVALID_STATE, TAG, "CSI controller not active");

    bool restart_direct_pipeline;
    portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
    restart_direct_pipeline = ctx->direct_h264_active;
    portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
    if (restart_direct_pipeline) {
        direct_h264_pipeline_deactivate(ctx);
    }

    esp_err_t ret = esp_cam_ctlr_stop(ctx->cam);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "csi rx stop failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* A failed pause may already have deactivated the pipeline while H.264
     * still owns a raw frame. Drain that owner before resetting buffer state,
     * even when this restart does not need to reactivate the pipeline. */
    ret = direct_h264_pipeline_drain(ctx, DIRECT_H264_DRAIN_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    MIPI_CSI_BRIDGE.csi_en.csi_brg_rst = 1;
    esp_rom_delay_us(2);
    MIPI_CSI_BRIDGE.csi_en.csi_brg_rst = 0;

    if (ctx->bridge.active_candidate && !ctx->bridge.active_candidate->output_rgb888 &&
        TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420) {
        /* A bridge reset clears CSI_CM's active line/frame state.  Restore
         * the input geometry and provide a new enable edge before rearming
         * VDMA, otherwise the next converted frame terminates early. */
        MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_en = 0;
        MIPI_CSI_BRIDGE.host_size_ctrl.csi_host_cm_vnum =
            ctx->capture_height - 1U;
        MIPI_CSI_BRIDGE.host_size_ctrl.csi_host_cm_hnum =
            ((uint32_t)ctx->capture_width * 16U / 32U) - 1U;
        esp_rom_delay_us(2);
        MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_en = 1;
    }

    while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
    }
    MIPI_CSI_BRIDGE.int_clr.val = 0x3f;
    MIPI_CSI_BRIDGE.int_ena.val = 0x3f;
#if CSI_BUF_AFULL_THRD_OVERRIDE > 0
    MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_afull_thrd = CSI_BUF_AFULL_THRD_OVERRIDE;
#endif
#if CSI_DMA_BURST_LEN_OVERRIDE > 0
    MIPI_CSI_BRIDGE.dma_req_cfg.dma_burst_len = CSI_DMA_BURST_LEN_OVERRIDE;
    MIPI_CSI_BRIDGE.dmablk_size.dmablk_size = csi_dma_block_bursts(ctx);
    MIPI_CSI_BRIDGE.dma_req_cfg.dma_cfg_upd_by_blk = 0;
    MIPI_CSI_BRIDGE.dma_req_cfg.csi_dma_flow_controller = 1;
#endif
    ctx->frames_finished = 0;
    ctx->frames_consumed = 0;
    ctx->last_received_size = 0;
    ctx->frame_buffer = ctx->capture_buffer_count ? ctx->capture_buffers[0] : NULL;
    csi_reset_buffer_state(ctx);

    ret = esp_cam_ctlr_start(ctx->cam);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "csi rx start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (restart_direct_pipeline) {
        direct_h264_pipeline_activate(ctx);
    }

    ESP_LOGI(TAG, "csi rx restarted without toggling TC358743 stream");
    return ESP_OK;
}

static esp_err_t p4_video_sleep_resume_capture(app_ctx_t *ctx)
{
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_ARG, TAG,
                        "invalid video sleep resume context");
    if (!ctx->cam) {
        ctx->video_sleep_resume_direct_pipeline = false;
        return ESP_OK;
    }

    /* HPD remains high while asleep, so the host may legitimately change
     * modes. Never restart old DMA geometry against a different signal. */
    tc358743_timings_t detected = {0};
    esp_err_t timing_ret = video_detect_timings(&ctx->bridge, &detected);
    if (timing_ret != ESP_OK || !timings_equal(&ctx->active_timings, &detected)) {
        if (timing_ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "HDMI mode changed during video sleep %ux%u@%u -> "
                     "%ux%u@%u; rebuilding",
                     ctx->active_timings.width, ctx->active_timings.height,
                     ctx->active_timings.fps, detected.width, detected.height,
                     detected.fps);
        } else {
            ESP_LOGW(TAG,
                     "HDMI signal unavailable while leaving video sleep: %s",
                     esp_err_to_name(timing_ret));
        }
        return csi_destroy(ctx);
    }

    ESP_RETURN_ON_ERROR(csi_restart_rx(ctx), TAG,
                        "restart CSI after video sleep failed");
    if (ctx->video_sleep_resume_direct_pipeline) {
        direct_h264_pipeline_activate(ctx);
    }
    ctx->video_sleep_resume_direct_pipeline = false;
    ESP_RETURN_ON_ERROR(tc_enable_stream(&ctx->bridge, true), TAG,
                        "unmute stream after video sleep failed");
    ESP_RETURN_ON_ERROR(csi_sync_rx_to_next_frame(ctx), TAG,
                        "frame-boundary sync after video sleep failed");
    return ESP_OK;
}

static esp_err_t csi_sync_rx_to_next_frame(app_ctx_t *ctx)
{
    const uint32_t frame_end_mask = BIT(28); /* ISP_HEADER_IDI_FRAME_INT */
    const int64_t deadline = esp_timer_get_time() + CSI_FRAME_SYNC_TIMEOUT_US;

    ISP.int_clr.val = frame_end_mask;
    while (!(ISP.int_raw.val & frame_end_mask)) {
        if (esp_timer_get_time() >= deadline) {
            /* The frame can end while this task is preempted between the
             * int_raw read and the deadline check. Read int_raw once more so
             * that a frame end is not reported as a timeout. */
            if (ISP.int_raw.val & frame_end_mask) break;
            ESP_LOGW(TAG, "timed out waiting for CSI input frame end int_raw=0x%08" PRIx32,
                     ISP.int_raw.val);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(20);
    }

    ESP_LOGI(TAG, "CSI input frame end observed; reset bridge FIFO and arm next frame");
    return csi_restart_rx(ctx);
}

static esp_err_t csi_restart_rx_phase(app_ctx_t *ctx, uint32_t attempt, const char *reason)
{
    ESP_LOGI(TAG, "frame-boundary CSI rearm attempt=%" PRIu32 " reason=%s",
             attempt, reason ? reason : "?");
    return csi_sync_rx_to_next_frame(ctx);
}

static esp_err_t tc_capture_one_with_rearms(app_ctx_t *ctx, uint32_t retries, const char *reason)
{
    esp_err_t last_ret = ESP_ERR_TIMEOUT;
    for (uint32_t attempt = 0; attempt < retries; attempt++) {
        last_ret = tc_capture_one(ctx);
        if (last_ret == ESP_OK || last_ret != ESP_ERR_TIMEOUT) {
            return last_ret;
        }
        ESP_LOGW(TAG, "capture timeout during %s, rearming CSI attempt=%" PRIu32 "/%" PRIu32,
                 reason ? reason : "capture", attempt + 1, retries);
        esp_err_t restart_ret = csi_restart_rx_phase(ctx, attempt + 1, reason);
        if (restart_ret != ESP_OK) {
            return restart_ret;
        }
    }
    return last_ret;
}

static esp_err_t isp_init_csi_passthrough(app_ctx_t *ctx)
{
    const tc358743_capture_candidate_t *candidate = ctx->bridge.active_candidate;
    bool has_hsync = candidate ? candidate->bridge_has_hsync : false;
    /* CSI_CM is between CSI HOST and ISP. The bypass input therefore has to
     * describe CSI_CM's output, not the sensor-side YUV422 stream. */
    isp_color_t passthrough_color = candidate && candidate->output_rgb888 ?
                                    ISP_COLOR_RGB888 :
                                    (TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420 ?
                                     ISP_COLOR_YUV420 : ISP_COLOR_YUV422);
    if (HDMI_DEBUG_FORCE_NO_HSYNC) {
        has_hsync = false;
    }

    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = passthrough_color,
        .output_data_color_type = passthrough_color,
        .yuv_range = ISP_COLOR_RANGE_LIMIT,
        .yuv_std = ISP_YUV_CONV_STD_BT601,
        .has_line_start_packet = has_hsync,
        .has_line_end_packet = has_hsync,
        .h_res = ctx->capture_width,
        .v_res = ctx->capture_height,
        .flags = {
            .bypass_isp = 1,
            .byte_swap_en = 0,
        },
    };

    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_config, &ctx->bridge.isp),
                        TAG, "esp_isp_new_processor failed");
    ISP.cntl.isp_en = 0;
    p4_log_isp_status(&ctx->bridge);
    return ESP_OK;
}

static bool tc_candidate_supports_mode(const tc358743_capture_candidate_t *candidate, const tc358743_timings_t *timings)
{
    if (!candidate || !timings) {
        return false;
    }
    if (candidate->lane_bps > 1000000000U) {
        return false;
    }

    /* The bridge emits one long packet per active line.  Its active-pixel
     * payload must finish inside the HDMI line period, so horizontal active
     * width is multiplied by total line count and frame rate.  Horizontal
     * blanking is time available to transmit that payload, not CSI data.
     * Vertical blanking still consumes line periods and is therefore included
     * through the total line count. */
    uint32_t width = timings->width;
    uint32_t height = timings->frame_height ? timings->frame_height : timings->height;
    uint32_t fps = timings->fps;
    if (HDMI_DEBUG_ASSUME_1080P30 &&
        ((timings->width == 1920 && timings->height == 1080) ||
         (timings->width == 1280 && timings->height == 720)) &&
        fps > 30) {
        fps = 30;
    }
    uint64_t bits_per_pixel = candidate->output_rgb888 ? 24 : 16;
    uint64_t required_bps = (uint64_t)width * height * fps * bits_per_pixel;
    uint64_t provided_bps = (uint64_t)candidate->lane_bps * candidate->data_lane_num;

    return provided_bps >= required_bps;
}

static bool tc_candidate_is_preferred_for_mode(const tc358743_capture_candidate_t *candidate,
                                               const tc358743_timings_t *timings)
{
    (void)timings;
    if (!candidate) {
        return false;
    }

    if (TC_H264_RAW_FORMAT == ESP_H264_RAW_FMT_BGR888) {
        return !candidate->use_colorbar &&
               candidate->output_rgb888 &&
               !candidate->use_full_timing &&
               !candidate->byte_swap &&
               candidate->clk_src == MIPI_CSI_PHY_CLK_SRC_PLL_F20M &&
               candidate->hs_freq_sel_override == 0xff &&
               candidate->data_lane_num == 2 &&
               candidate->lane_bps == 594000000;
    }

    return !candidate->use_colorbar &&
           !candidate->output_rgb888 &&
           !candidate->use_full_timing &&
           !candidate->byte_swap &&
           candidate->clk_src == MIPI_CSI_PHY_CLK_SRC_PLL_F20M &&
           candidate->hs_freq_sel_override == 0xff &&
           candidate->data_lane_num == 2 &&
           candidate->lane_bps >= 357000000;
}

/* The WebRTC peer outlives video rebuilds. The large video buffers are the
 * video pool and a rebuild allocates none of them, but the encoder's control
 * state and the CSI descriptors still come from Internal RAM. When the
 * rebuild still runs out of memory, release the peer once and try again; the
 * browser reconnects. */
static bool video_release_peer_once(bool *released)
{
    if (*released) {
        return false;
    }
    *released = true;
    return webrtc_transport_release_peer_and_wait(3000) == ESP_OK;
}

static esp_err_t tc_try_candidates_held(app_ctx_t *ctx,
                                        const tc358743_timings_t *timings)
{
    bool forced_candidate_supported = false;
    bool peer_released = false;
    if (HDMI_DEBUG_FORCE_CANDIDATE[0]) {
        for (size_t i = 0; i < sizeof(s_candidates) / sizeof(s_candidates[0]); i++) {
            if (strcmp(s_candidates[i].name, HDMI_DEBUG_FORCE_CANDIDATE) == 0) {
                forced_candidate_supported =
                    tc_candidate_supports_mode(&s_candidates[i], timings);
                break;
            }
        }
        if (!forced_candidate_supported) {
            ESP_LOGI(TAG,
                     "forced candidate=%s cannot carry %ux%u@%u; selecting by bandwidth",
                     HDMI_DEBUG_FORCE_CANDIDATE,
                     timings->width, timings->height, timings->fps);
        }
    }

    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sizeof(s_candidates) / sizeof(s_candidates[0]); i++) {
            const tc358743_capture_candidate_t *candidate = &s_candidates[i];
            bool preferred = tc_candidate_is_preferred_for_mode(candidate, timings);

            bool force_matched = HDMI_DEBUG_FORCE_CANDIDATE[0] &&
                                 strcmp(candidate->name, HDMI_DEBUG_FORCE_CANDIDATE) == 0;

            if (HDMI_DEBUG_YUV_ONLY && !force_matched &&
                (candidate->output_rgb888 || candidate->use_colorbar)) {
                continue;
            }
            if (HDMI_DEBUG_FORCE_CANDIDATE[0] && forced_candidate_supported &&
                strcmp(candidate->name, HDMI_DEBUG_FORCE_CANDIDATE) != 0) {
                continue;
            }
            if (pass == 0 && !preferred) {
                continue;
            }
            if (pass == 1 && preferred) {
                continue;
            }

            if (!tc_candidate_supports_mode(candidate, timings)) {
                ESP_LOGI(TAG, "skip candidate=%s insufficient bandwidth for %ux%u@%u",
                         candidate->name, timings->width, timings->height, timings->fps);
                continue;
            }

            ESP_LOGI(TAG, "try candidate=%s %s %ux%" PRIu32 "Mbps",
                     candidate->name,
                     candidate->output_rgb888 ? "rgb888" : "yuv422",
                     candidate->data_lane_num,
                     candidate->lane_bps / 1000000);

retry_candidate:
            ESP_RETURN_ON_ERROR(csi_destroy(ctx), TAG, "csi destroy failed");
            size_t video_working_set = 0U;
            esp_err_t ret = video_mode_prepare(
                ctx, timings, candidate, &video_working_set);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "candidate=%s memory model failed: %s",
                         candidate->name, esp_err_to_name(ret));
                continue;
            }
            jetkvm_memory_policy_video_rebuild_t video_transaction = {0};
            ret = jetkvm_memory_policy_video_rebuild_begin(
                candidate->name, video_working_set, &video_transaction);
            /* A peer that began opening before the hold keeps its PSRAM
             * ticket until its next check gives up or it materializes. */
            for (int wait = 0; ret == ESP_ERR_INVALID_STATE &&
                               wait < VIDEO_ADMISSION_WAITS; ++wait) {
                vTaskDelay(pdMS_TO_TICKS(VIDEO_ADMISSION_WAIT_MS));
                ret = jetkvm_memory_policy_video_rebuild_begin(
                    candidate->name, video_working_set, &video_transaction);
            }
            if (ret == ESP_ERR_NO_MEM && video_release_peer_once(&peer_released)) {
                goto retry_candidate;
            }
            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "candidate=%s video transaction denied: %s",
                         candidate->name, esp_err_to_name(ret));
                continue;
            }
            ret = tc_start_streaming(ctx, timings, candidate);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "candidate=%s start failed: %s", candidate->name, esp_err_to_name(ret));
                esp_err_t cleanup_ret = csi_destroy(ctx);
                if (cleanup_ret != ESP_OK) {
                    ESP_LOGE(TAG,
                             "candidate=%s failed-start cleanup failed: %s",
                             candidate->name, esp_err_to_name(cleanup_ret));
                }
                esp_err_t finish_ret =
                    jetkvm_memory_policy_video_rebuild_finish(
                        &video_transaction, false);
                if (finish_ret != ESP_OK) {
                    return finish_ret;
                }
                if (ret == ESP_ERR_NO_MEM &&
                    video_release_peer_once(&peer_released)) {
                    goto retry_candidate;
                }
                continue;
            }
            while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            }

            ret = tc_capture_one_with_rearms(ctx, CSI_REARM_CAPTURE_RETRIES, "candidate-lock");
            if (ret == ESP_OK) {
                esp_err_t finish_ret =
                    jetkvm_memory_policy_video_rebuild_finish(
                        &video_transaction, true);
                if (finish_ret != ESP_OK) {
                    return finish_ret;
                }
                if (!candidate->output_rgb888 &&
                    csi_output_matches_h264_raw(TC_H264_RAW_FORMAT)) {
                    direct_h264_pipeline_activate(ctx);
                }
                ESP_LOGI(TAG, "candidate locked: %s", candidate->name);
                return ESP_OK;
            }

            ESP_LOGW(TAG, "candidate=%s capture failed: %s", candidate->name, esp_err_to_name(ret));
            tc_log_top_interrupts(&ctx->bridge);
            tc_log_sub_interrupts(&ctx->bridge);
            tc_log_video_output(&ctx->bridge);
            log_frame_buffer_probe(ctx, "timeout");
            tc_log_csi_status(&ctx->bridge);
            p4_log_csi_status(&ctx->bridge);
            p4_log_isp_status(&ctx->bridge);
            esp_err_t disable_ret = tc_enable_stream(&ctx->bridge, false);
            if (disable_ret != ESP_OK) {
                ESP_LOGE(TAG,
                         "candidate=%s failed-capture stream stop failed: %s",
                         candidate->name, esp_err_to_name(disable_ret));
            }
            esp_err_t cleanup_ret = csi_destroy(ctx);
            if (cleanup_ret != ESP_OK) {
                ESP_LOGE(TAG,
                         "candidate=%s failed-capture cleanup failed: %s",
                         candidate->name, esp_err_to_name(cleanup_ret));
            }
            esp_err_t finish_ret =
                jetkvm_memory_policy_video_rebuild_finish(
                    &video_transaction, false);
            if (finish_ret != ESP_OK) {
                return finish_ret;
            }
        }
    }

    ESP_RETURN_ON_ERROR(csi_destroy(ctx), TAG, "csi destroy after candidates failed");
    return ESP_ERR_NOT_FOUND;
}

/* No peer opens during the rebuild: it would take the memory the next mode
 * is admitted against, also right after video_release_peer_once(). */
static esp_err_t tc_try_candidates(app_ctx_t *ctx, const tc358743_timings_t *timings)
{
    webrtc_transport_hold_peer_open(true);
    esp_err_t ret = tc_try_candidates_held(ctx, timings);
    webrtc_transport_hold_peer_open(false);
    return ret;
}

static bool IRAM_ATTR csi_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    (void)handle;
    if (ctx->use_driver_backup) {
        trans->buffer = NULL;
        trans->buflen = 0;
        return false;
    }

    if (!ctx->capture_buffer_count) {
        trans->buffer = NULL;
        trans->buflen = 0;
        return false;
    }

    uint8_t index = ctx->capture_next_buffer % ctx->capture_buffer_count;
    bool found = false;
    /* The encoder claims READY under the pipeline mux. Take both locks in
     * the same order as the EOF callback before considering READY for reuse. */
    portENTER_CRITICAL_ISR(&s_direct_h264_pipeline_mux);
    portENTER_CRITICAL_ISR(&s_csi_buffer_mux);
    uint32_t available = ctx->free_buffers & csi_all_buffer_mask(ctx);
    available &= ~(ctx->locked_buffers | ctx->inflight_buffers | ctx->ready_buffers);
    for (uint8_t tries = 0; tries < ctx->capture_buffer_count; tries++) {
        uint32_t bit = 1u << index;
        if (available & bit) {
            ctx->free_buffers &= ~bit;
            ctx->inflight_buffers |= bit;
            found = true;
            break;
        }
        index = (uint8_t)((index + 1) % ctx->capture_buffer_count);
    }
    if (!found && ctx->direct_h264_active &&
        !ctx->direct_h264_overlap_enabled) {
        int8_t ready_index = ctx->direct_h264_ready_index;
        if (ready_index >= 0 &&
            (uint8_t)ready_index < ctx->capture_buffer_count &&
            ready_index != ctx->direct_h264_encoding_index) {
            uint32_t bit = 1u << (uint8_t)ready_index;
            direct_h264_slot_t *slot =
                &ctx->direct_h264_slots[(uint8_t)ready_index];
            if (slot->generation == ctx->direct_h264_generation &&
                slot->capture_complete && !slot->cancelled &&
                !slot->overlap_candidate &&
                (ctx->locked_buffers & bit) != 0 &&
                ((ctx->free_buffers | ctx->inflight_buffers |
                  ctx->ready_buffers) & bit) == 0) {
                /* The CSI driver requests a replacement before reporting
                 * the just-completed frame. With two/three DMA descriptors,
                 * one encoding frame and one READY frame, four/five buffers
                 * leave no FREE slot when encoding falls behind. Keeping the old
                 * READY would suppress the newer EOF and can then expire the
                 * old frame too. Recycle only this unconsumed, completed
                 * READY: the driver can now hand off its newer completion.
                 * No encoded reference picture or live DMA is overwritten. */
                ctx->direct_h264_ready_index = DIRECT_H264_INVALID_INDEX;
                slot->cancelled = true;
                ctx->locked_buffers &= ~bit;
                ctx->inflight_buffers |= bit;
                ctx->capture_received_sizes[(uint8_t)ready_index] = 0;
                ctx->capture_buffer_sequences[(uint8_t)ready_index] = 0;
                ctx->capture_buffer_timestamps_us[(uint8_t)ready_index] = 0;
                ctx->metrics.direct_ready_replaced++;
                index = (uint8_t)ready_index;
                found = true;
            }
        }
    }
    if (found) {
        ctx->capture_next_buffer = (uint8_t)((index + 1) % ctx->capture_buffer_count);
    } else {
        ctx->csi_no_free_buffers++;
        ctx->metrics.capture_nofree_drops++;
    }
    portEXIT_CRITICAL_ISR(&s_csi_buffer_mux);
    portEXIT_CRITICAL_ISR(&s_direct_h264_pipeline_mux);

    if (!found) {
        trans->buffer = NULL;
        trans->buflen = 0;
        return false;
    }

    trans->buffer = ctx->capture_buffers[index];
    /* The active DMA transfer remains sized by the CSI controller's current
     * geometry, but direct YUV capture buffers are provisioned for the largest
     * accepted mode.  Advertising the allocation capacity here also makes
     * cache maintenance cover the complete allocation.  This protects the
     * PSRAM heap metadata while an HDMI source is changing from 720p to 1080p
     * before software has rebuilt the controller for the new timing. */
    trans->buflen = ctx->capture_buffer_capacity;
    return false;
}

static bool IRAM_ATTR csi_trans_started(esp_cam_ctlr_handle_t handle,
                                        const esp_cam_ctlr_trans_t *trans,
                                        void *user_data)
{
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    (void)handle;
    int8_t index = csi_buffer_index_from_ptr(ctx, trans ? trans->buffer : NULL);
    if (index >= 0 && ctx->direct_h264_overlap_enabled) {
        return direct_h264_pipeline_start_from_isr(
            ctx, index, esp_timer_get_time());
    }
    return false;
}

static bool IRAM_ATTR csi_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    (void)handle;
    BaseType_t high_task_woken = pdFALSE;
    /* Do not clear bridge interrupts here; the IDF CSI driver manages them.
     * Clearing them in this callback stops later DMA transfers from
     * completing, so capture stops after the first frame. */
    int8_t index = csi_buffer_index_from_ptr(ctx, trans->buffer);
    if (index >= 0) {
        uint32_t bit = 1u << (uint8_t)index;
        int64_t csi_dma_eof_timestamp_us = esp_timer_get_time();
        bool direct_handoff;
        uint32_t capture_sequence;
        portENTER_CRITICAL_ISR(&s_direct_h264_pipeline_mux);
        direct_handoff = ctx->direct_h264_active;
        portENTER_CRITICAL_ISR(&s_csi_buffer_mux);
        ctx->inflight_buffers &= ~bit;
        if (direct_handoff) {
            ctx->locked_buffers |= bit;
        } else {
            ctx->ready_buffers |= bit;
        }
        ctx->capture_received_sizes[(uint8_t)index] = trans->received_size;
        ctx->capture_sequence++;
        capture_sequence = ctx->capture_sequence;
        ctx->capture_buffer_sequences[(uint8_t)index] = capture_sequence;
        ctx->capture_buffer_timestamps_us[(uint8_t)index] =
            csi_dma_eof_timestamp_us;
        ctx->frame_buffer = trans->buffer;
        ctx->last_received_size = trans->received_size;
        ctx->current_capture_sequence = capture_sequence;
        ctx->current_capture_timestamp_us = csi_dma_eof_timestamp_us;
        ctx->frames_finished++;
        ctx->metrics.capture_frames++;
        portEXIT_CRITICAL_ISR(&s_csi_buffer_mux);
        if (direct_handoff && !direct_h264_pipeline_submit_locked_from_isr(
                ctx, index, capture_sequence, csi_dma_eof_timestamp_us,
                &high_task_woken)) {
            csi_release_consumed_buffer_from_isr(ctx, index);
        }
        portEXIT_CRITICAL_ISR(&s_direct_h264_pipeline_mux);
    }
    if (index >= 0 && ctx->capture_task) {
        vTaskNotifyGiveFromISR(ctx->capture_task, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static esp_err_t csi_init(app_ctx_t *ctx, const tc358743_timings_t *timings, uint32_t lane_bps_per_lane)
{
    esp_err_t ret = ESP_OK;
    size_t frame_buffer_alignment = 0;
    const tc358743_capture_candidate_t *candidate = ctx->bridge.active_candidate;
    bool bridge_has_hsync = candidate ? candidate->bridge_has_hsync : false;
    uint16_t capture_width = candidate && candidate->use_full_timing ? timings->frame_width : timings->width;
    uint16_t capture_height = candidate && candidate->use_full_timing ? timings->frame_height : timings->height;

    if (HDMI_DEBUG_FORCE_NO_HSYNC) {
        bridge_has_hsync = false;
    }
    if (candidate && candidate->use_full_timing && HDMI_DEBUG_CAPTURE_ACTIVE_DE) {
        capture_width = timings->width;
        capture_height = timings->height;
    }

    if (capture_width > FRAME_WIDTH_MAX || capture_height > FRAME_HEIGHT_MAX) {
        ESP_LOGW(TAG, "mode %ux%u@%u exceeds current RAM budget %ux%u",
                 capture_width, capture_height, timings->fps, FRAME_WIDTH_MAX, FRAME_HEIGHT_MAX);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_FALSE(candidate, ESP_ERR_INVALID_STATE, TAG, "capture candidate not set");
    ESP_RETURN_ON_FALSE(capture_width > 0 && capture_height > 0, ESP_ERR_INVALID_ARG, TAG, "invalid capture size");
    const bool direct_h264_capture = !candidate->output_rgb888 &&
                                     csi_output_matches_h264_raw(TC_H264_RAW_FORMAT);
    const bool direct_1080p30 = direct_h264_capture &&
                                timings->width == 1920 &&
                                timings->height == 1080 &&
                                timings->fps <= 30;
    const bool compact_csi_ring =
        csi_use_compact_ring(ctx, timings, direct_h264_capture);
    ctx->direct_h264_overlap_enabled =
        !ctx->preview_only_active && DIRECT_H264_OVERLAP_ENABLE &&
        direct_1080p30;
    ESP_LOGI(TAG, "csi:timings de=%ux%u frame=%ux%u capture=%ux%u hsync=%d",
             timings->width, timings->height,
             timings->frame_width, timings->frame_height,
             capture_width, capture_height, bridge_has_hsync);

    ESP_LOGI(TAG, "csi:init");

    ctx->capture_width = capture_width;
    ctx->capture_height = capture_height;
    if (candidate->output_rgb888) {
        ctx->frame_buffer_size = (size_t)capture_width * capture_height * 3;
    } else if (TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420) {
        ctx->frame_buffer_size = (size_t)capture_width * capture_height * 3 / 2;
    } else {
        ctx->frame_buffer_size = (size_t)capture_width * capture_height * 2;
    }
    ESP_RETURN_ON_FALSE(ctx->frame_buffer_size <= FRAME_BUFFER_MAX_BYTES, ESP_ERR_NOT_SUPPORTED, TAG, "frame buffer too large");
    ctx->capture_buffer_capacity = CSI_CAPTURE_CAPACITY(ctx->frame_buffer_size);
    if (direct_h264_capture) {
        if (TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420) {
            ctx->capture_buffer_capacity =
                (size_t)FRAME_WIDTH_MAX * FRAME_HEIGHT_MAX * 3 / 2;
        } else {
            ctx->capture_buffer_capacity =
                (size_t)FRAME_WIDTH_MAX * FRAME_HEIGHT_MAX * 2;
        }
    }
    ESP_RETURN_ON_FALSE(ctx->capture_buffer_capacity >= ctx->frame_buffer_size &&
                        ctx->capture_buffer_capacity <= FRAME_BUFFER_MAX_BYTES,
                        ESP_ERR_NOT_SUPPORTED, TAG, "capture buffer capacity invalid");
    ctx->frame_buffer = NULL;
    ctx->capture_buffer_count = 0;
    ctx->capture_next_buffer = 0;
    ctx->free_buffers = 0;
    ctx->inflight_buffers = 0;
    ctx->ready_buffers = 0;
    ctx->locked_buffers = 0;
    ctx->csi_no_free_buffers = 0;
    memset((void *)ctx->capture_received_sizes, 0, sizeof(ctx->capture_received_sizes));
    ctx->frame_buffer_owned_by_driver = false;
    if (!ctx->use_driver_backup) {
        ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &frame_buffer_alignment);
        if (ret != ESP_OK) {
            return ret;
        }
        ESP_LOGI(TAG, "csi:align=%u fbsize=%u capacity=%u",
                 (unsigned)frame_buffer_alignment,
                 (unsigned)ctx->frame_buffer_size,
                 (unsigned)ctx->capture_buffer_capacity);
        const size_t capture_buffer_target = compact_csi_ring ?
            CSI_FRAME_BUFFER_COUNT_COMPACT_1080P30 : direct_h264_capture ?
            (direct_1080p30 ? CSI_FRAME_BUFFER_COUNT_DIRECT_1080P30 :
                              CSI_FRAME_BUFFER_COUNT_DIRECT_HIGH_RATE) :
            CSI_FRAME_BUFFER_COUNT_DEFAULT;
        /* The mode uses the first slots of the video pool. video_mode_prepare()
         * refused a mode whose frame does not fit a slot. */
        ESP_RETURN_ON_FALSE(ctx->capture_buffer_capacity <= ctx->capture_buffers_capacity,
                            ESP_ERR_INVALID_STATE, TAG, "capture slot too small");
        for (size_t i = 0; i < capture_buffer_target; i++) {
            ESP_LOGI(TAG, "csi:slot[%d] ptr=%p", (int)i, ctx->capture_buffers[i]);
            ESP_RETURN_ON_FALSE(ctx->capture_buffers[i], ESP_ERR_INVALID_STATE, TAG,
                                "video pool slot missing");
            ESP_RETURN_ON_ERROR(
                dma_cache_fastpath_register(
                    ctx->capture_buffers[i], ctx->capture_buffer_capacity,
                    &ctx->capture_cache_fastpaths[i]),
                TAG, "CSI cache fast-path registration failed");
        }
        ctx->capture_buffer_count = capture_buffer_target;
        ctx->frame_buffer = ctx->capture_buffers[0];
        csi_reset_buffer_state(ctx);
        ESP_LOGI(TAG, "csi:buffers ready count=%d free=0x%08" PRIx32,
                 ctx->capture_buffer_count, ctx->free_buffers);
    }

    if (!ctx->mipi_ldo) {
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = JETKVM_MIPI_LDO_CHAN_ID,
            .voltage_mv = JETKVM_MIPI_LDO_VOLTAGE_MV,
        };
        ret = esp_ldo_acquire_channel(&ldo_cfg, &ctx->mipi_ldo);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    ESP_LOGI(TAG, "csi:ldo");

    const bool use_raw_dtype = !candidate->output_rgb888 && TC_CSI_USE_RAW_DATA_TYPE;
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .clk_src = candidate ? candidate->clk_src : MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = capture_width,
        .v_res = capture_height,
        .lane_bit_rate_mbps = lane_bps_per_lane / 1000000,
        .input_data_color_type = candidate->output_rgb888 ? CAM_CTLR_COLOR_RGB888 : TC_CSI_INPUT_YUV422_COLOR,
        .output_data_color_type = candidate->output_rgb888 ? CAM_CTLR_COLOR_RGB888 : TC_CSI_OUTPUT_YUV_COLOR,
        .data_lane_num = candidate->data_lane_num,
        .input_8bit_swap_en = !candidate->output_rgb888 && TC_CSI_INPUT_8BIT_SWAP,
        .input_16bit_swap_en = !candidate->output_rgb888 && TC_CSI_INPUT_16BIT_SWAP,
        .byte_swap_en = candidate->output_rgb888 ? candidate->byte_swap :
                        TC_CSI_OUTPUT_BYTE_SWAP,
        .bk_buffer_dis = direct_h264_capture && !ctx->use_driver_backup,
        .queue_items = 1,
    };
    if (use_raw_dtype) {
        csi_config.input_data_color_type = 0;
        csi_config.output_data_color_type = 0;
        csi_config.input_8bit_swap_en = 0;
        csi_config.input_16bit_swap_en = 0;
        csi_config.data_type.bits_per_pixel = 16;
        csi_config.data_type.data_type_min = TC_CSI_YUV422_DATA_TYPE;
        csi_config.data_type.data_type_max = TC_CSI_YUV422_DATA_TYPE;
    }

    ESP_LOGI(TAG, "csi:buffer policy app=%u driver_backup=%s",
             (unsigned)ctx->capture_buffer_count,
             csi_config.bk_buffer_dis ? "disabled" : "enabled");

    ESP_LOGI(TAG, "create CSI %ux%u lanes=%u rate=%dMbps mode=%s in=%s out=%s input8swap=%d input16swap=%d outputswap=%d h264=%s",
             capture_width, capture_height, csi_config.data_lane_num,
             csi_config.lane_bit_rate_mbps,
             use_raw_dtype ? "raw-dtype-0x1e-16bpp" : "color-format",
             candidate->output_rgb888 ? "rgb888" : cam_yuv_color_name(TC_CSI_INPUT_YUV422_COLOR),
             candidate->output_rgb888 ? "rgb888" :
             cam_yuv_color_name(TC_CSI_OUTPUT_YUV_COLOR),
             csi_config.input_8bit_swap_en,
             csi_config.input_16bit_swap_en,
             csi_config.byte_swap_en,
             candidate->output_rgb888 ? "n/a" : h264_raw_fmt_name(TC_H264_RAW_FORMAT));
    const size_t dma_ring_count = compact_csi_ring ?
        P4_CSI_DMA_RING_COMPACT : P4_CSI_DMA_RING_DEFAULT;
    ESP_LOGI(TAG, "csi:DMA ring=%u", (unsigned)dma_ring_count);
    ESP_RETURN_ON_ERROR(
        p4_cam_new_csi_ctlr_with_dma_ring(
            &csi_config, dma_ring_count, &ctx->cam),
        TAG, "p4_cam_new_csi_ctlr_with_dma_ring failed");
    ESP_RETURN_ON_ERROR(
        p4_cam_ctlr_csi_register_frame_started_callback(
            ctx->cam, csi_trans_started, ctx),
        TAG, "CSI frame-start callback registration failed");
    ESP_LOGI(TAG, "csi:new");
    if (candidate->hs_freq_sel_override != 0xff) {
        p4_override_hs_freq_sel(candidate->hs_freq_sel_override);
    }
    /* Without these explicit bridge writes, clk_en and int_ena stay 0 and no
     * DMA completion callback arrives. */
    MIPI_CSI_BRIDGE.clk_en.clk_en = 1;
    MIPI_CSI_BRIDGE.host_ctrl.csi_enableclk = 1;
    MIPI_CSI_BRIDGE.host_ctrl.csi_cfg_clk_en = 1;
    MIPI_CSI_BRIDGE.int_clr.val = 0x3f;
    MIPI_CSI_BRIDGE.int_ena.val = 0x3f;
#if CSI_BUF_AFULL_THRD_OVERRIDE > 0
    MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_afull_thrd = CSI_BUF_AFULL_THRD_OVERRIDE;
    ESP_LOGI(TAG, "csi:bridge afull_thrd=%u", (unsigned)CSI_BUF_AFULL_THRD_OVERRIDE);
#endif
#if CSI_DMA_BURST_LEN_OVERRIDE > 0
    MIPI_CSI_BRIDGE.dma_req_cfg.dma_burst_len = CSI_DMA_BURST_LEN_OVERRIDE;
    ESP_LOGI(TAG, "csi:bridge dma_burst_len=%u", (unsigned)CSI_DMA_BURST_LEN_OVERRIDE);
    MIPI_CSI_BRIDGE.dmablk_size.dmablk_size = csi_dma_block_bursts(ctx);
    MIPI_CSI_BRIDGE.dma_req_cfg.dma_cfg_upd_by_blk = 0;
    MIPI_CSI_BRIDGE.dma_req_cfg.csi_dma_flow_controller = 1;
    ESP_LOGI(TAG, "csi:bridge flow=bridge update_by_frame=1 dmablk_size=%u",
             (unsigned)csi_dma_block_bursts(ctx));
#endif
    if (candidate->output_rgb888) {
        MIPI_CSI_BRIDGE.data_type_cfg.data_type_min = candidate->dtype_min;
        MIPI_CSI_BRIDGE.data_type_cfg.data_type_max = candidate->dtype_max;
    } else if (TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420) {
        /* CSI_CM changes the downstream pixel sideband from packed YUV422
         * (0x1e) to a YUV420 data type.  Keep the TRM's complete valid image
         * range so frame sync and converted pixels reach CSI_Bridge. */
        MIPI_CSI_BRIDGE.data_type_cfg.data_type_min = 0x18;
        MIPI_CSI_BRIDGE.data_type_cfg.data_type_max = 0x2f;
    } else {
        MIPI_CSI_BRIDGE.data_type_cfg.data_type_min = TC_CSI_YUV422_DATA_TYPE;
        MIPI_CSI_BRIDGE.data_type_cfg.data_type_max = TC_CSI_YUV422_DATA_TYPE;
    }
    /* CSI_Bridge sees CSI_CM's YUV420 output as 12 bits per actual image row.
     * HADR is expressed in 64-bit IDI words; VADR remains the source height. */
    const bool bridge_yuv420 = !candidate->output_rgb888 &&
                               TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420;
    uint32_t bridge_output_bpp = candidate->output_rgb888 ? 24 :
                                 (bridge_yuv420 ? 12 : 16);
    uint32_t bridge_hadr = bridge_yuv420 ?
                           ((uint32_t)capture_width * bridge_output_bpp) / 64U :
                           (((uint32_t)capture_width * bridge_output_bpp + 63U) / 64U);
    uint32_t bridge_vadr = capture_height;
    MIPI_CSI_BRIDGE.frame_cfg.hadr_num = bridge_hadr;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num = bridge_vadr;
    MIPI_CSI_BRIDGE.frame_cfg.has_hsync_e = bridge_has_hsync ? 1 : 0;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num_check = 0;
    ESP_LOGI(TAG, "csi:bridge output frame hadr=%" PRIu32
             " vadr=%u bpp=%" PRIu32 " hsync=%d",
             bridge_hadr, bridge_vadr, bridge_output_bpp,
             bridge_has_hsync);
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = csi_get_new_trans,
        .on_trans_finished = csi_trans_finished,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(ctx->cam, &cbs, ctx), TAG, "esp_cam_ctlr_register_event_callbacks failed");
    if (ctx->use_driver_backup) {
        const void *driver_fb = NULL;
        size_t driver_fb_len = 0;
        ESP_RETURN_ON_ERROR(esp_cam_ctlr_get_frame_buffer(ctx->cam, 1, &driver_fb),
                            TAG, "esp_cam_ctlr_get_frame_buffer failed");
        ESP_RETURN_ON_ERROR(esp_cam_ctlr_get_frame_buffer_len(ctx->cam, &driver_fb_len),
                            TAG, "esp_cam_ctlr_get_frame_buffer_len failed");
        ctx->frame_buffer = (void *)driver_fb;
        ctx->frame_buffer_size = driver_fb_len;
        ctx->capture_buffer_capacity = driver_fb_len;
        ctx->frame_buffer_owned_by_driver = true;
        ESP_LOGI(TAG, "use driver backup buffer=%p size=%u", ctx->frame_buffer, (unsigned)ctx->frame_buffer_size);
    }
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(ctx->cam), TAG, "esp_cam_ctlr_enable failed");
    ESP_LOGI(TAG, "csi:ena");
    ESP_RETURN_ON_ERROR(isp_init_csi_passthrough(ctx), TAG, "isp_init_csi_passthrough failed");
    ESP_LOGI(TAG, "csi:isp-bypass format=%s",
             candidate->output_rgb888 ? "RGB888" :
             (TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420 ?
              "YUV420" : "YUV422"));
    if (!candidate->output_rgb888 &&
        TC_CSI_OUTPUT_YUV_COLOR == CAM_CTLR_COLOR_YUV420) {
        /* CSI_CM and ISP share the receive pipeline.  Give the converter a
         * fresh enable edge after ISP configuration and before CSI starts. */
        MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_en = 0;
        MIPI_CSI_BRIDGE.int_clr.val = 0x3f;
        esp_rom_delay_us(2);
        MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_en = 1;
        ESP_LOGI(TAG, "csi:cm rearmed after ISP init ctrl=0x%08" PRIx32,
                 MIPI_CSI_BRIDGE.host_cm_ctrl.val);
    }
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(ctx->cam), TAG, "esp_cam_ctlr_start failed");
    ESP_LOGI(TAG, "csi:start");

    return ESP_OK;
}

static esp_err_t tc_start_streaming(app_ctx_t *ctx, const tc358743_timings_t *timings, const tc358743_capture_candidate_t *candidate)
{
    ESP_RETURN_ON_ERROR(tc_enable_stream(&ctx->bridge, false), TAG, "disable stream failed");
    ESP_RETURN_ON_FALSE(candidate && candidate->timing_preset, ESP_ERR_INVALID_ARG, TAG, "invalid capture candidate");
    ctx->bridge.active_candidate = candidate;
    ESP_LOGI(TAG, "st:phy");
    ESP_RETURN_ON_ERROR(tc_reset_phy(&ctx->bridge), TAG, "reset phy failed");
    ESP_LOGI(TAG, "st:pll");
    ESP_RETURN_ON_ERROR(tc_set_pll(&ctx->bridge, ctx->bridge.refclk_hz, candidate->lane_bps), TAG, "set_pll failed");
    ESP_LOGI(TAG, "st:csi");
    ESP_RETURN_ON_ERROR(tc_set_csi(&ctx->bridge, candidate->timing_preset), TAG, "set_csi failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(&ctx->bridge, VOUT_SET3, 1, candidate->use_extcnt ? MASK_VOUT_EXTCNT : 0), TAG, "vout_set3 failed");
    if (candidate->use_colorbar) {
        ESP_RETURN_ON_ERROR(tc_set_colorbar_output(&ctx->bridge), TAG, "set_colorbar_output failed");
    } else if (candidate->output_rgb888) {
        ESP_RETURN_ON_ERROR(tc_set_rgb888_output(&ctx->bridge), TAG, "set_rgb888_output failed");
    } else {
        ESP_RETURN_ON_ERROR(tc_set_yuv422_output(&ctx->bridge), TAG, "set_yuv422_output failed");
    }
    ESP_LOGI(TAG, "st:fmt");
    vTaskDelay(pdMS_TO_TICKS(10));
    uint16_t encode_width = candidate->use_full_timing ? timings->frame_width : timings->width;
    uint16_t encode_height = candidate->use_full_timing ? timings->frame_height : timings->height;
    if (candidate->use_full_timing && HDMI_DEBUG_CAPTURE_ACTIVE_DE) {
        encode_width = timings->width;
        encode_height = timings->height;
    }
    uint8_t encode_fps = h264_mode_fps(encode_width, encode_height, timings->fps);
    video_metrics_begin_mode(ctx, encode_width, encode_height, encode_fps);
    bool prealloc_h264 = candidate->output_rgb888 ?
                         (TC_H264_RAW_FORMAT == ESP_H264_RAW_FMT_BGR888) : true;
    if (ctx->preview_only_active) {
        /* The screen test mode owns completed CSI frames directly.  No H.264
         * encoder instance or output buffer is created in this mode. */
        h264_hw_deinit(ctx);
        ctx->h264_width = encode_width;
        ctx->h264_height = encode_height;
        ctx->h264_fps = encode_fps;
        ctx->h264_pic_type = TC_H264_RAW_FORMAT;
        ESP_LOGI(TAG, "st:screen-only %ux%u@%u H264 disabled",
                 encode_width, encode_height, encode_fps);
    } else if (prealloc_h264) {
        ESP_LOGI(TAG, "st:h264-prealloc %ux%u@%u %s", encode_width, encode_height, encode_fps,
                 h264_raw_fmt_name(TC_H264_RAW_FORMAT));
        ESP_RETURN_ON_ERROR(h264_hw_ensure(ctx, encode_width, encode_height,
                                           encode_fps, TC_H264_RAW_FORMAT),
                            TAG, "h264 prealloc failed");
    }
    ESP_LOGI(TAG, "st:init");
    ESP_RETURN_ON_ERROR(csi_init(ctx, timings, candidate->lane_bps), TAG, "csi_init failed");
    p4_wait_stopstate(&ctx->bridge, candidate->data_lane_num, 50);
    ESP_LOGI(TAG, "st:on");
    ESP_RETURN_ON_ERROR(tc_enable_stream(&ctx->bridge, true), TAG, "enable stream failed");
    ESP_RETURN_ON_ERROR(csi_sync_rx_to_next_frame(ctx), TAG, "frame-boundary CSI sync failed");
    ctx->active_timings = *timings;
    ctx->active_preset = candidate->timing_preset;
    ctx->frame_counter = 0;
    ctx->last_signature = 0;
    ctx->raw_serial_dumped = false;
    ctx->frames_finished = 0;
    ctx->frames_consumed = 0;
    ctx->last_received_size = 0;
    ESP_LOGI(TAG, "st:settle %" PRIu32 "ms rearm=%d",
             (uint32_t)CSI_STREAM_SETTLE_MS, CSI_REARM_AFTER_STREAM_ENABLE);
    vTaskDelay(pdMS_TO_TICKS(CSI_STREAM_SETTLE_MS));
#if CSI_REARM_AFTER_STREAM_ENABLE
    ESP_RETURN_ON_ERROR(csi_restart_rx_phase(ctx, 0, "stream-settle"), TAG, "stream settle CSI rearm failed");
    vTaskDelay(pdMS_TO_TICKS(20));
#endif
    ESP_LOGI(TAG, "st:dump");
    tc_log_video_output(&ctx->bridge);
    tc_log_top_interrupts(&ctx->bridge);
    tc_log_sub_interrupts(&ctx->bridge);
    p4_log_csi_status(&ctx->bridge);
    p4_log_isp_status(&ctx->bridge);
    /* Capture buffers remain cache-invalid and device-exclusive.  A CPU
     * framebuffer probe here would defeat the DMA cache fast path. */
    return ESP_OK;
}

static esp_err_t tc_capture_one(app_ctx_t *ctx)
{
    const tc358743_capture_candidate_t *candidate = ctx->bridge.active_candidate;
    esp_err_t ret = ESP_OK;
    int8_t consumed_idx = -1;
    uint32_t capture_sequence = 0;
    int64_t capture_timestamp_us = 0;
    uint32_t stale_dropped = 0;
#define TC_CAPTURE_DONE(err_) do { ret = (err_); goto out; } while (0)

    bool direct_handoff_active;
    portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
    direct_handoff_active = ctx->direct_h264_active;
    portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
    if (direct_handoff_active) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE,
                                             pdMS_TO_TICKS(FRAME_WAIT_MS));
        if (notified == 0) {
            ESP_LOGW(TAG,
                     "direct frame wait timeout capture_seq=%" PRIu32
                     " free=0x%08" PRIx32 " inflight=0x%08" PRIx32
                     " ready=0x%08" PRIx32 " locked=0x%08" PRIx32,
                     ctx->capture_sequence, ctx->free_buffers,
                     ctx->inflight_buffers, ctx->ready_buffers,
                     ctx->locked_buffers);
            return ESP_ERR_TIMEOUT;
        }
        uint32_t latest_sequence;
        int64_t latest_eof_timestamp_us;
        portENTER_CRITICAL(&s_csi_buffer_mux);
        latest_sequence = ctx->current_capture_sequence;
        latest_eof_timestamp_us = ctx->current_capture_timestamp_us;
        portEXIT_CRITICAL(&s_csi_buffer_mux);
        uint32_t advanced = latest_sequence >= ctx->frames_consumed ?
            latest_sequence - ctx->frames_consumed : 0;
        ctx->frames_consumed = latest_sequence;
        ctx->frame_counter += advanced;
        ctx->consecutive_timeouts = 0;
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }
        uint32_t frame_log_seconds =
            ctx->direct_h264_overlap_enabled ? 30U : 2U;
        uint32_t frame_log_interval = ctx->h264_fps ?
            (uint32_t)ctx->h264_fps * frame_log_seconds : 60;
        if (latest_sequence == 1 ||
            (latest_sequence % frame_log_interval) == 0) {
            ESP_LOGI(TAG,
                     "direct frame capture_seq=%" PRIu32
                     " eof_us=%" PRId64 " advanced=%" PRIu32
                     " finished=%" PRIu32,
                     latest_sequence, latest_eof_timestamp_us, advanced,
                     ctx->frames_finished);
        }
        return ESP_OK;
    }

    if ((ctx->ready_buffers & csi_all_buffer_mask(ctx)) == 0) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FRAME_WAIT_MS));
        if (notified == 0) {
            ESP_LOGW(TAG, "frame wait timeout brg_depth=%" PRIu32 " int_raw=0x%08" PRIx32
                     " int_st=0x%08" PRIx32 " dtype=0x%08" PRIx32
                     " buf free=0x%08" PRIx32 " inflight=0x%08" PRIx32
                     " ready=0x%08" PRIx32 " locked=0x%08" PRIx32
                     " nofree=%" PRIu32,
                     MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_depth,
                     MIPI_CSI_BRIDGE.int_raw.val,
                     MIPI_CSI_BRIDGE.int_st.val,
                     MIPI_CSI_BRIDGE.data_type_cfg.val,
                     ctx->free_buffers,
                     ctx->inflight_buffers,
                     ctx->ready_buffers,
                     ctx->locked_buffers,
                     ctx->csi_no_free_buffers);
            return ESP_ERR_TIMEOUT;
        }
    }
    size_t received = 0;
    consumed_idx = csi_take_ready_buffer(ctx, &received, &capture_sequence,
                                         &capture_timestamp_us, &stale_dropped);
    if (consumed_idx < 0) {
        ESP_LOGW(TAG, "frame ready mask empty finished=%" PRIu32 " consumed=%" PRIu32
                 " free=0x%08" PRIx32 " inflight=0x%08" PRIx32
                 " ready=0x%08" PRIx32 " locked=0x%08" PRIx32,
                 ctx->frames_finished, ctx->frames_consumed,
                 ctx->free_buffers, ctx->inflight_buffers,
                 ctx->ready_buffers, ctx->locked_buffers);
        return ESP_ERR_TIMEOUT;
    }
    ctx->frames_consumed += 1 + stale_dropped;
    ctx->consecutive_timeouts = 0;
    while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
    }
    ctx->frame_counter++;
    ctx->current_capture_sequence = capture_sequence;
    ctx->current_capture_timestamp_us = capture_timestamp_us ? capture_timestamp_us : esp_timer_get_time();
    received = received ? received : ctx->frame_buffer_size;

    /* p4_csi invalidates a completed DMA buffer before invoking its finished
     * callback. Repeating a whole-frame M2C sync here costs enough to miss the
     * two accepted 30 Hz mode frame budgets. */
    /* Do not sample pixels on the CPU: the registered CSI buffer remains
     * cache-invalid from CSI DMA through H.264 DMA. The real capture sequence
     * is sufficient for the periodic liveness log. */
    uint32_t sig = capture_sequence;
    uint32_t frame_log_interval = ctx->h264_fps ? (uint32_t)ctx->h264_fps * 2 : 60;
    if (ctx->frame_counter == 1 || (ctx->frame_counter % frame_log_interval) == 0) {
        ESP_LOGI(TAG, "frame=%" PRIu32 " capture_seq=%" PRIu32 " size=%u signature=0x%08" PRIx32
                 " finished=%" PRIu32 " stale_drop=%" PRIu32,
                 ctx->frame_counter, capture_sequence, (unsigned)received, sig,
                 ctx->frames_finished, stale_dropped);
    }
    ctx->last_signature = sig;

    /* Candidate lock intentionally takes one ordinary READY frame before
     * enabling ISR direct handoff.  The ISR may already finish a newer frame
     * after csi_take_ready_buffer() locked consumed_idx, so the process-global
     * frame_buffer pointer is no longer an ownership token here.  Completion
     * size plus the locked index are sufficient to validate this probe; the
     * direct task will consume exact per-index pointers after activation. */
    bool direct_pipeline_active;
    portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
    direct_pipeline_active = ctx->direct_h264_active;
    portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
    if (!direct_pipeline_active && candidate && !candidate->output_rgb888 &&
        csi_output_matches_h264_raw(TC_H264_RAW_FORMAT)) {
        TC_CAPTURE_DONE(ESP_OK);
    }

    /* Raw pixel dump on first frame - scan for first non-black line */
    if (H264_DEBUG_PIXEL_INSPECTION && ctx->frame_counter == 1 && ctx->frame_buffer && received >= 256) {
        const uint8_t *fb = (const uint8_t *)ctx->frame_buffer;
        size_t bpp = (candidate && candidate->output_rgb888) ? 3 : 2;
        size_t stride = (size_t)ctx->capture_width * bpp;
        /* Find first line that is not all 0x10/0x80 (YUV black) */
        int first_data_line = -1;
        for (int line = 0; line < (int)ctx->capture_height && (size_t)(line + 1) * stride <= received; line++) {
            const uint8_t *lp = fb + (size_t)line * stride;
            bool is_black = true;
            for (size_t i = 0; i < 16 && i < stride; i += 2) {
                if (lp[i] != 0x10 || lp[i+1] != 0x80) { is_black = false; break; }
            }
            if (!is_black) {
                first_data_line = line;
                break;
            }
        }
        ESP_LOGI(TAG, "RAW_SCAN first_data_line=%d capture=%ux%u stride=%u",
                 first_data_line, ctx->capture_width, ctx->capture_height, (unsigned)stride);
        /* Dump a few lines around the first data line and at key positions */
        int dump_lines[] = {0, 1, first_data_line > 0 ? first_data_line - 1 : 0,
                           first_data_line, first_data_line + 1,
                           (int)ctx->capture_height / 2,
                           (int)ctx->capture_height - 2, (int)ctx->capture_height - 1};
        for (int di = 0; di < 8; di++) {
            int line = dump_lines[di];
            if (line < 0 || line >= (int)ctx->capture_height) continue;
            size_t off = (size_t)line * stride;
            if (off + 16 > received) continue;
            const uint8_t *lp = fb + off;
            ESP_LOGI(TAG, "  L%03d: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     line, lp[0], lp[1], lp[2], lp[3], lp[4], lp[5], lp[6], lp[7],
                     lp[8], lp[9], lp[10], lp[11], lp[12], lp[13], lp[14], lp[15]);
        }

    }

#if RAW_SERIAL_DUMP_AFTER_FRAME > 0
    if (ctx->frame_buffer && !ctx->raw_serial_dumped &&
        ctx->frame_counter >= RAW_SERIAL_DUMP_AFTER_FRAME) {
        size_t dump_len = received;
        if (dump_len == 0 || dump_len > ctx->frame_buffer_size) {
            dump_len = ctx->frame_buffer_size;
        }
        if (dump_len > RAW_SERIAL_DUMP_MAX_BYTES) {
            dump_len = RAW_SERIAL_DUMP_MAX_BYTES;
        }
        esp_cache_msync((void *)ctx->frame_buffer, dump_len,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        ctx->raw_serial_dumped = true;
        ESP_LOGI(TAG, "RAW_DMA_DUMP_META frame=%" PRIu32 " fmt=%s width=%u height=%u size=%u",
                 ctx->frame_counter,
                 candidate && candidate->output_rgb888 ? "RGB888" :
                 cam_yuv_color_name(csi_effective_output_yuv_color()),
                 ctx->capture_width, ctx->capture_height, (unsigned)dump_len);
        h264_debug_dump_hex_block("SERIAL_RAW_DMA_START",
                                  "SERIAL_RAW_DMA_END",
                                  (const uint8_t *)ctx->frame_buffer,
                                  (uint32_t)dump_len);
    }
#endif

    /* H264 HW directly from the CSI DMA output buffer. Do not do CPU pixel
     * conversion here; the CSI output format must already match the encoder.
     */
    if (ctx->frame_buffer && ctx->bridge.active_candidate && !ctx->bridge.active_candidate->output_rgb888 &&
        H264_DEBUG_ENCODE_INTERVAL_FRAMES > 0 &&
        (ctx->frame_counter % H264_DEBUG_ENCODE_INTERVAL_FRAMES) == 0) {
        int8_t li = -1;
        for (uint8_t i = 0; i < ctx->capture_buffer_count; i++) {
            if (ctx->capture_buffers[i] == ctx->frame_buffer) { li = (int8_t)i; break; }
        }
        /* csi_take_ready_buffer() already locked this exact buffer. */
        if (li != consumed_idx) {
            ESP_LOGE(TAG, "capture buffer ownership mismatch taken=%d frame=%d",
                     consumed_idx, li);
            TC_CAPTURE_DONE(ESP_ERR_INVALID_STATE);
        }

        bool capture_is_h264_raw = csi_output_matches_h264_raw(TC_H264_RAW_FORMAT);
        cam_ctlr_color_t effective_capture_color = csi_effective_output_yuv_color();
        const char *capture_fmt = cam_yuv_color_name(effective_capture_color);
        const uint8_t *capture_src = (const uint8_t *)ctx->frame_buffer;
        const uint8_t *src = capture_src;
        size_t raw_size = h264_raw_frame_size(ctx->capture_width, ctx->capture_height, TC_H264_RAW_FORMAT);

        if (!capture_is_h264_raw) {
            ESP_LOGW(TAG, "H264YUV: DMA format mismatch capture=%s encoder=%s; skip without software conversion",
                     capture_fmt, h264_raw_fmt_name(TC_H264_RAW_FORMAT));
            TC_CAPTURE_DONE(ESP_OK);
        }

        if (ctx->frame_buffer_size < raw_size) {
            ESP_LOGW(TAG, "H264YUV: CSI %s buffer too small (%u < %u)",
                     capture_fmt,
                     (unsigned)ctx->frame_buffer_size, (unsigned)raw_size);
            TC_CAPTURE_DONE(ESP_OK);
        }

        uint8_t y_min = 0;
        uint8_t y_max = 0;
        bool h264_debug_detail = H264_DEBUG_PIXEL_INSPECTION &&
                                 (ctx->frame_counter <= 3 ||
                                 (H264_DEBUG_SAMPLE_INTERVAL_FRAMES > 0 &&
                                  (ctx->frame_counter % H264_DEBUG_SAMPLE_INTERVAL_FRAMES) == 0) ||
                                 (H264_SERIAL_DUMP_ENABLE && !ctx->h264_serial_dumped &&
                                  ctx->frame_counter >= H264_SERIAL_DUMP_AFTER_FRAME));
        if (h264_debug_detail) {
            h264_raw_luma_range(src, raw_size, TC_H264_RAW_FORMAT, &y_min, &y_max);
        }
        if (h264_debug_detail && effective_capture_color != CAM_CTLR_COLOR_YUV420) {
            log_yuv422_frame_samples("H264YUV capture sample", capture_src,
                                     ctx->capture_width, ctx->capture_height,
                                     ctx->frame_buffer_size);
        }

        if (h264_debug_detail) {
            ESP_LOGI(TAG,
                     "H264YUV: frame=%" PRIu32 " capture=%s encode=%s %ux%u raw=%u y_range=%u..%u",
                     ctx->frame_counter,
                     capture_fmt,
                     h264_raw_fmt_name(TC_H264_RAW_FORMAT),
                     ctx->capture_width, ctx->capture_height, (unsigned)raw_size, y_min, y_max);
            ESP_LOGI(TAG, "H264YUV: capture line0 %s bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                     capture_fmt,
                     capture_src[0], capture_src[1], capture_src[2], capture_src[3],
                     capture_src[4], capture_src[5], capture_src[6], capture_src[7]);
            ESP_LOGI(TAG, "H264YUV: encode line0 %s bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                     h264_raw_fmt_name(TC_H264_RAW_FORMAT),
                     src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7]);
        }

        bool direct_pipeline_active;
        portENTER_CRITICAL(&s_direct_h264_pipeline_mux);
        direct_pipeline_active = ctx->direct_h264_active;
        portEXIT_CRITICAL(&s_direct_h264_pipeline_mux);
        if (!direct_pipeline_active) {
            /* tc_try_candidates validates one ordinary READY frame before it
             * atomically enables ISR direct handoff. That probe is not part
             * of the encoded stream. */
            TC_CAPTURE_DONE(ESP_OK);
        }

        esp_err_t submit_ret = direct_h264_pipeline_submit(
            ctx, consumed_idx, capture_sequence,
            ctx->current_capture_timestamp_us);
        if (submit_ret == ESP_OK) {
            /* Ownership of this exact LOCKED bit moved to the direct H.264
             * queue/encoder task.  The common exit must not release it. */
            consumed_idx = -1;
        } else {
            ESP_LOGW(TAG,
                     "direct H264 submit failed capture_seq=%" PRIu32 " ret=%s",
                     capture_sequence, esp_err_to_name(submit_ret));
        }
        TC_CAPTURE_DONE(ESP_OK);
    }

    /* H264 HW from RGB888 CSI output. The hardware encoder accepts BGR888 on
     * ESP32-P4 rev >= 3.0; if the CSI byte order is RGB, this only swaps red
     * and blue visually, which is still enough to validate frame integrity.
     */
    if (ctx->h264_enc && ctx->h264_output_buf && ctx->frame_buffer &&
        ctx->bridge.active_candidate && ctx->bridge.active_candidate->output_rgb888 &&
        ctx->frame_counter == 2) {
        int8_t li = -1;
        for (uint8_t i = 0; i < ctx->capture_buffer_count; i++) {
            if (ctx->capture_buffers[i] == ctx->frame_buffer) { li = (int8_t)i; break; }
        }
        ctx->locked_buffers = (li >= 0) ? (1u << li) : 0;

        esp_cache_msync((void *)ctx->frame_buffer, ctx->frame_buffer_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

        size_t raw_size = h264_raw_frame_size(ctx->capture_width, ctx->capture_height, TC_H264_RAW_FORMAT);
        const uint8_t *src = (const uint8_t *)ctx->frame_buffer;
        if (TC_H264_RAW_FORMAT != ESP_H264_RAW_FMT_BGR888 || ctx->frame_buffer_size < raw_size) {
            ctx->locked_buffers = 0;
            ESP_LOGW(TAG, "H264RGB: unsupported fmt=%s fb=%u raw=%u",
                     h264_raw_fmt_name(TC_H264_RAW_FORMAT),
                     (unsigned)ctx->frame_buffer_size, (unsigned)raw_size);
            TC_CAPTURE_DONE(ESP_OK);
        }

        ESP_LOGI(TAG, "H264RGB: frame=%" PRIu32 " encode=%s %ux%u raw=%u",
                 ctx->frame_counter, h264_raw_fmt_name(TC_H264_RAW_FORMAT),
                 ctx->capture_width, ctx->capture_height, (unsigned)raw_size);
        ESP_LOGI(TAG, "H264RGB: src line0: %02x %02x %02x %02x %02x %02x %02x %02x",
                 src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7]);

        esp_h264_enc_in_frame_t in_f = {
            .raw_data = { .buffer = (uint8_t *)src, .len = raw_size },
            .pts = ctx->frame_counter,
        };
        uint8_t *h264_output = ctx->h264_output_buf;
        esp_h264_enc_out_frame_t out_f = {
            .raw_data = { .buffer = h264_output, .len = ctx->h264_output_len },
        };

        int64_t t0 = esp_timer_get_time();
        esp_h264_err_t hr = esp_h264_enc_process(ctx->h264_enc, &in_f, &out_f);
        int64_t t1 = esp_timer_get_time();
        ctx->locked_buffers = 0;
        ESP_LOGI(TAG, "H264RGB: ret=%d type=%d len=%" PRIu32 " time=%" PRId64 "ms fmt=%s",
                 hr, out_f.frame_type, out_f.length, (t1 - t0) / 1000,
                 h264_raw_fmt_name(ctx->h264_pic_type));

        if (hr == ESP_H264_ERR_OK && out_f.length > 0 && !ctx->h264_serial_dumped) {
            ctx->h264_serial_dumped = true;
            if (out_f.length <= H264_SERIAL_DUMP_MAX_BYTES) {
                ESP_LOGI(TAG, "SERIAL_H264_START size=%" PRIu32, out_f.length);
                const uint8_t *p = out_f.raw_data.buffer;
                size_t remaining = out_f.length;
                while (remaining > 0) {
                    char hexl[129];
                    size_t chunk = remaining > 64 ? 64 : remaining;
                    for (size_t i = 0; i < chunk; i++) snprintf(&hexl[i*2], 3, "%02x", p[i]);
                    hexl[chunk*2] = '\0';
                    printf("%s\n", hexl);
                    p += chunk;
                    remaining -= chunk;
                }
                ESP_LOGI(TAG, "SERIAL_H264_END");
            } else {
                ESP_LOGW(TAG, "SERIAL_H264_SKIP size=%" PRIu32 " max=%u",
                         out_f.length, H264_SERIAL_DUMP_MAX_BYTES);
            }
        }
    }

    TC_CAPTURE_DONE(ESP_OK);

out:
    csi_release_consumed_buffer(ctx, consumed_idx);
#undef TC_CAPTURE_DONE
    return ret;
}

/* A pass that expects video at the detected rate fps. Returns true when it
 * reset the bridge, which tears capture down: the caller starts its loop
 * over. */
static bool hdmi_health_check(app_ctx_t *ctx, hdmi_health_t *health,
                              hdmi_hpd_retry_t *hpd_retry,
                              const tc358743_timings_t *timings,
                              bool streamable)
{
    const hdmi_health_action_t action = hdmi_health_sample(
        health, esp_timer_get_time(),
        __atomic_load_n(&ctx->metrics.capture_frames, __ATOMIC_RELAXED),
        timings->fps, streamable);
    if (action == HDMI_HEALTH_GIVE_UP) {
        ESP_LOGI(TAG,
                 "HDMI recovery: no usable video after %d bridge resets; no "
                 "more resets until HPD rises again",
                 HDMI_HEALTH_MAX_RESETS);
        return false;
    }
    if (action != HDMI_HEALTH_RESET) {
        return false;
    }
    ESP_LOGW(TAG,
             "HDMI recovery: %ux%u@%u gave %" PRIu32 " frames in the last "
             "window%s; resetting the bridge (reset %" PRIu32 "/%d)",
             timings->width, timings->height, timings->fps, health->frames,
             health->unstreamable ? ", mode not streamable" : "",
             health->resets, HDMI_HEALTH_MAX_RESETS);
    ESP_ERROR_CHECK(tc_enable_stream(&ctx->bridge, false));
    ESP_ERROR_CHECK(csi_destroy(ctx));
    /* As at boot, a bridge that fails its setup restarts the Mini: no
     * later pass would set it up again. */
    ESP_ERROR_CHECK(hdmi_bridge_reset(ctx));
    /* The EDID write of the reset raised HPD again. */
    hdmi_hpd_retry_arm(hpd_retry);
    return true;
}

static void hdmi_capture_task(void *arg)
{
    (void)arg;
    app_ctx_t ctx = {0};
    tc358743_timings_t detected = {0};
    uint32_t no_signal_count = 0;
    uint32_t loop_trace = 0;
    int64_t next_active_timing_check_us = 0;
    int64_t next_hdmi_connection_check_us = 0;
    bool host_display_advertised = true;
    /* tc_initial_setup() below raises HPD after it writes the EDID. */
    hdmi_hpd_retry_t hpd_retry = {.armed = true};
    bool hpd_retry_pulse_low = false;
    hdmi_health_t health = {0};
    int64_t video_sleep_idle_since_us = 0;

    ctx.capture_task = xTaskGetCurrentTaskHandle();
    ctx.bridge.log_tag = TAG;
    ctx.direct_h264_ready_index = DIRECT_H264_INVALID_INDEX;
    ctx.direct_h264_encoding_index = DIRECT_H264_INVALID_INDEX;
    bool manual_preview = VIDEO_MODE_DEFAULT_PREVIEW_ONLY != 0;
    ctx.preview_only_active = manual_preview || !__atomic_load_n(
        &s_network_available, __ATOMIC_ACQUIRE);
    __atomic_store_n(&ctx.preview_only_requested,
                     manual_preview, __ATOMIC_RELEASE);
    ctx.use_driver_backup = HDMI_DEBUG_USE_DRIVER_BACKUP;
    for (size_t i = 0; i < VIDEO_POOL_CAPTURE_SLOTS; i++) {
        ctx.capture_buffers[i] = s_video_pool.capture[i];
    }
    ctx.capture_buffers_capacity = VIDEO_POOL_CAPTURE_SLOT_BYTES;
    ESP_LOGI(TAG, "capture debug force_candidate=%s driver_backup=%d",
             HDMI_DEBUG_FORCE_CANDIDATE[0] ? HDMI_DEBUG_FORCE_CANDIDATE : "(none)",
             ctx.use_driver_backup);

    h264_hw_enable_clock();
    ESP_LOGI(TAG, "h264hw: clock ready; encoder follows detected HDMI mode");
    ctx.screen_preview_queue = xQueueCreate(1, sizeof(screen_preview_job_t));
    ESP_ERROR_CHECK(ctx.screen_preview_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t preview_task_created = xTaskCreatePinnedToCoreWithCaps(
        screen_preview_task, "screen_preview", SCREEN_PREVIEW_TASK_STACK,
        &ctx, tskIDLE_PRIORITY + 2, &ctx.screen_preview_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(preview_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#if !CONFIG_JETKVM_SCREEN_BUTTONS
    BaseType_t button_task_created = xTaskCreatePinnedToCore(
        video_mode_button_task, "video_mode_button",
        VIDEO_MODE_BUTTON_TASK_STACK, &ctx, tskIDLE_PRIORITY + 1, NULL, 0);
    ESP_ERROR_CHECK(button_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#endif
    BaseType_t direct_encode_task_created = xTaskCreatePinnedToCore(
        direct_h264_pipeline_encode_task, "direct_h264", DIRECT_H264_TASK_STACK,
        &ctx, tskIDLE_PRIORITY + 7, &ctx.direct_h264_encode_task,
        VIDEO_MEDIA_CORE);
    ESP_ERROR_CHECK(direct_encode_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "csi_cm: direct hardware YUV422 -> O_UYY_E_VYY ready");

    ESP_LOGI(TAG, "init:gpio");
    tc_gpio_init();
    ESP_LOGI(TAG, "init:hwreset");
    tc_hw_reset(&ctx.bridge);
    ESP_LOGI(TAG, "init:i2c");
    tc_i2c_init(&ctx.bridge);
    ESP_LOGI(TAG, "init:probe");
    ESP_ERROR_CHECK(tc_probe_chip(&ctx.bridge));
    jetkvm_video_edid_preset_t stored_edid =
        JETKVM_DEFAULT_VIDEO_EDID_PRESET;
    esp_err_t stored_edid_error =
        jetkvm_settings_get_video_edid_preset(&stored_edid);
    if (stored_edid_error != ESP_OK || !p4_edid_descriptor(stored_edid)) {
        ESP_LOGW(TAG, "stored EDID unavailable; using 1080p30 default: %s",
                 esp_err_to_name(stored_edid_error));
        stored_edid = JETKVM_DEFAULT_VIDEO_EDID_PRESET;
    }
    const p4_edid_descriptor_t *initial_edid =
        p4_edid_descriptor(stored_edid);
    ESP_LOGI(TAG, "init:setup refclk=%" PRIu32, TC358743_REFCLK_HZ);
    ESP_ERROR_CHECK(tc_initial_setup(&ctx.bridge, TC358743_REFCLK_HZ,
                                     initial_edid));
    ESP_LOGI(TAG, "init:done");
    tc_log_signal_status(&ctx.bridge);
    if (p4_edid_control_mark_ready(stored_edid)) {
        p4_device_set_edid_control_available(true);
        ESP_LOGI(TAG, "EDID control ready preset=%s", initial_edid->label);
    }

    while (1) {
        /* Poll before policy branches that skip capture during idle or sleep. */
        video_update_hdmi_connection(&ctx.bridge,
                                     &next_hdmi_connection_check_us);
        /* OS Services guest video: while the guest holds the receiver the
         * preview, host display, sleep and EDID preset branches wait. An
         * EDID write leaves HPD high and physical capture torn down. */
        if (guest_display_service(&ctx)) {
            host_display_advertised = true;
            /* The guest's EDID writes pulse HPD, and so does the restore. */
            hdmi_hpd_retry_arm(&hpd_retry);
            hdmi_health_arm(&health);
            hpd_retry_pulse_low = false;
            video_sleep_idle_since_us = 0;
            __atomic_store_n(&s_video_sleep_active, false, __ATOMIC_RELEASE);
            no_signal_count = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        bool requested_preview = __atomic_load_n(
            &ctx.preview_only_requested, __ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&s_network_available, __ATOMIC_ACQUIRE);
        if (requested_preview != ctx.preview_only_active) {
            if (ctx.cam) {
                ESP_ERROR_CHECK(tc_enable_stream(&ctx.bridge, false));
                ESP_ERROR_CHECK(csi_destroy(&ctx));
            }
            ctx.preview_only_active = requested_preview;
            ctx.screen_preview_logged = false;
            ctx.screen_preview_last_enqueue_us = 0;
            video_sleep_idle_since_us = 0;
            __atomic_store_n(&s_video_sleep_active, false,
                             __ATOMIC_RELEASE);
            if (requested_preview) {
                if (!host_display_advertised && p4_host_display_may_raise()) {
                    ESP_ERROR_CHECK(tc_set_hpd_output(&ctx.bridge, true));
                    host_display_advertised = true;
                }
            } else {
                (void)jetkvm_screen_preview_stop();
            }
            ESP_LOGI(TAG, "video mode switched to %s; rebuilding capture",
                     requested_preview ? "screen-only" : "h264-only");
            hdmi_health_pause(&health);
            no_signal_count = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (ctx.preview_only_active && !host_display_advertised &&
            p4_host_display_may_raise()) {
            ESP_ERROR_CHECK(tc_set_hpd_output(&ctx.bridge, true));
            host_display_advertised = true;
        }
        esp_err_t host_display_ret =
            ctx.preview_only_active ? ESP_OK :
            p4_host_display_sync(&ctx, &host_display_advertised);
        if (host_display_ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "host display policy sync failed: %s; failing open",
                     esp_err_to_name(host_display_ret));
            esp_err_t fail_open_ret = tc_set_hpd_output(&ctx.bridge, true);
            if (fail_open_ret != ESP_OK) {
                ESP_LOGE(TAG,
                         "host display fail-open HPD retry failed: %s",
                         esp_err_to_name(fail_open_ret));
                host_display_advertised = false;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            host_display_advertised = true;
        }
        if (!host_display_advertised) {
            /* HPD rises again when the policy shows the display, or at the
             * end of a recovery pulse, which keeps its count. */
            if (!hpd_retry_pulse_low) {
                hdmi_hpd_retry_arm(&hpd_retry);
                hdmi_health_arm(&health);
            }
            hdmi_health_pause(&health);
            video_sleep_idle_since_us = 0;
            if (__atomic_exchange_n(&s_video_sleep_active, false,
                                    __ATOMIC_ACQ_REL)) {
                ESP_LOGI(TAG,
                         "HDMI video sleep exited reason=host-display-hidden");
            }
            no_signal_count = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        hpd_retry_pulse_low = false;
        esp_err_t video_sleep_ret = ctx.preview_only_active ? ESP_OK :
            p4_video_sleep_sync(&ctx, &video_sleep_idle_since_us);
        if (video_sleep_ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "HDMI video sleep transition failed: %s; failing open",
                     esp_err_to_name(video_sleep_ret));
            __atomic_store_n(&s_video_sleep_active, false,
                             __ATOMIC_RELEASE);
            video_sleep_idle_since_us = esp_timer_get_time();
            hdmi_health_pause(&health);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (__atomic_load_n(&s_video_sleep_active, __ATOMIC_ACQUIRE)) {
            hdmi_health_pause(&health);
            no_signal_count = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* During an OTA install a queued EDID waits: applying it would
         * rebuild the capture that the install holds suspended. */
        if (!__atomic_load_n(&s_capture_suspended, __ATOMIC_ACQUIRE) &&
            p4_edid_control_process(&ctx.bridge)) {
            /* The RPC task still owns transaction_lock while it commits the
             * new preset or queues a hardware rollback after an NVS failure.
             * Service such a rollback before capture teardown can wait on
             * that RPC peer, then give the response a short send window. */
            const TickType_t transaction_started = xTaskGetTickCount();
            while (xTaskGetTickCount() - transaction_started <
                   pdMS_TO_TICKS(3000)) {
                if (p4_edid_control_lock(0)) {
                    p4_edid_control_unlock();
                    break;
                }
                if (!p4_edid_control_process(&ctx.bridge)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            /* EDID programming muted the bridge even if the source keeps the
             * same mode. Rebuild explicitly after the RPC transaction ends;
             * do not wait for a frame timeout to discover the muted stream. */
            ESP_ERROR_CHECK(csi_destroy(&ctx));
            /* tc_program_edid() held HPD low and raised it again. */
            hdmi_hpd_retry_arm(&hpd_retry);
            hdmi_health_arm(&health);
            no_signal_count = 0;
            next_active_timing_check_us = esp_timer_get_time() + 750000;
            continue;
        }
        if (ctx.cam) {
            /* An OTA install holds the capture suspended: its sequential
             * flash writes disable the cache in bursts that stall CSI frame
             * completion, and a running capture would hit its frame-wait
             * timeout and rebuild the pipeline, tearing the WebRTC peer down
             * mid-install. Idle without touching the pipeline; the encode
             * path is already paused, so the frozen capture is not sent.
             * consecutive_timeouts is left untouched and clears on the first
             * frame captured after resume. */
            if (__atomic_load_n(&s_capture_suspended, __ATOMIC_ACQUIRE)) {
                hdmi_health_pause(&health);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (ctx.preview_only_active) {
                hdmi_health_pause(&health);
            } else if (hdmi_health_check(&ctx, &health, &hpd_retry,
                                         &ctx.active_timings,
                                         !ctx.h264_mode_refused)) {
                no_signal_count = 0;
                continue;
            }
            int64_t now_us = esp_timer_get_time();
            if (now_us >= next_active_timing_check_us) {
                tc358743_timings_t active_detected = {0};
                next_active_timing_check_us = now_us + (int64_t)ACTIVE_TIMING_CHECK_MS * 1000;
                esp_err_t timing_ret = video_detect_timings(&ctx.bridge, &active_detected);
                if (timing_ret != ESP_OK || !timings_equal(&ctx.active_timings, &active_detected)) {
                    if (timing_ret == ESP_OK) {
                        ESP_LOGI(TAG,
                                 "HDMI mode changed %ux%u@%u -> %ux%u@%u; rebuilding hardware pipeline",
                                 ctx.active_timings.width, ctx.active_timings.height, ctx.active_timings.fps,
                                 active_detected.width, active_detected.height, active_detected.fps);
                    } else {
                        ESP_LOGW(TAG, "active HDMI signal lost: %s; rebuilding hardware pipeline",
                                 esp_err_to_name(timing_ret));
                    }
                    ESP_ERROR_CHECK(tc_enable_stream(&ctx.bridge, false));
                    ESP_ERROR_CHECK(csi_destroy(&ctx));
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
            }

            esp_err_t ret = tc_capture_one(&ctx);
            video_metrics_maybe_log(&ctx);
            if (ret != ESP_OK) {
                /* A suspend requested while this capture was already waiting
                 * surfaces here as a timeout; idle rather than rebuild, the
                 * same as the loop-top gate, so an in-flight frame cannot
                 * tear the OTA session down through csi_destroy. */
                if (__atomic_load_n(&s_capture_suspended, __ATOMIC_ACQUIRE)) {
                    hdmi_health_pause(&health);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                bool timing_changed_during_timeout = false;
                if (ret == ESP_ERR_TIMEOUT) {
                    ctx.consecutive_timeouts++;
                    /* A resolution switch normally first appears as a frame
                     * timeout.  Re-check the receiver before rearming CSI so
                     * the old geometry is never deliberately restarted on a
                     * newly enlarged input frame. */
                    tc358743_timings_t timeout_detected = {0};
                    esp_err_t timeout_timing_ret =
                        video_detect_timings(&ctx.bridge, &timeout_detected);
                    timing_changed_during_timeout =
                        timeout_timing_ret != ESP_OK ||
                        !timings_equal(&ctx.active_timings, &timeout_detected);
                    if (timing_changed_during_timeout) {
                        if (timeout_timing_ret == ESP_OK) {
                            ESP_LOGI(TAG,
                                     "HDMI mode changed during capture %ux%u@%u -> %ux%u@%u; rebuilding without CSI rearm",
                                     ctx.active_timings.width,
                                     ctx.active_timings.height,
                                     ctx.active_timings.fps,
                                     timeout_detected.width,
                                     timeout_detected.height,
                                     timeout_detected.fps);
                        } else {
                            ESP_LOGW(TAG,
                                     "HDMI signal lost during capture: %s; rebuilding without CSI rearm",
                                     esp_err_to_name(timeout_timing_ret));
                        }
                    }
                }
                ESP_LOGW(TAG, "capture failed: %s consecutive_timeouts=%" PRIu32,
                         esp_err_to_name(ret), ctx.consecutive_timeouts);
                tc_log_video_output(&ctx.bridge);
                tc_log_csi_status(&ctx.bridge);
                tc_log_sub_interrupts(&ctx.bridge);
                p4_log_csi_status(&ctx.bridge);
                p4_log_isp_status(&ctx.bridge);
                if (ret == ESP_ERR_TIMEOUT && !timing_changed_during_timeout &&
                    ctx.consecutive_timeouts <= 4) {
                    esp_err_t restart_ret = csi_restart_rx_phase(&ctx, ctx.consecutive_timeouts, "active-timeout");
                    if (restart_ret == ESP_OK) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }
                    ESP_LOGW(TAG, "soft CSI restart failed: %s; rebuilding pipeline",
                             esp_err_to_name(restart_ret));
                }
                ESP_ERROR_CHECK(tc_enable_stream(&ctx.bridge, false));
                ESP_ERROR_CHECK(csi_destroy(&ctx));
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            continue;
        }

        if (loop_trace < 4) {
            ESP_LOGI(TAG, "loop:%" PRIu32 ":poll", loop_trace);
        }
        tc_poll_interrupts(&ctx.bridge);
        if (loop_trace < 4) {
            ESP_LOGI(TAG, "loop:%" PRIu32 ":detect", loop_trace);
        }
        esp_err_t ret = video_detect_timings(&ctx.bridge, &detected);
        const bool expect_video = !ctx.preview_only_active &&
            !__atomic_load_n(&s_capture_suspended, __ATOMIC_ACQUIRE);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "waiting for HDMI signal: %s", esp_err_to_name(ret));
            /* A signal whose timings stay out of range is a bad state of
             * the bridge as well; without a signal the HPD retry acts. */
            if (ret != ESP_ERR_INVALID_RESPONSE || !expect_video) {
                hdmi_health_pause(&health);
            } else if (hdmi_health_check(&ctx, &health, &hpd_retry,
                                         &detected, false)) {
                no_signal_count = 0;
                continue;
            }
            if (no_signal_count == 0) {
                ESP_ERROR_CHECK(tc_enable_stream(&ctx.bridge, false));
                ESP_ERROR_CHECK(csi_destroy(&ctx));
            }
            if ((no_signal_count % 3) == 0) {
                tc_log_signal_status(&ctx.bridge);
            }
            no_signal_count++;
            loop_trace++;
            const int64_t wait_start_us = hpd_retry.wait_start_us;
            const int64_t now_us = esp_timer_get_time();
            const hdmi_hpd_retry_action_t retry_action = hdmi_hpd_retry_due(
                &hpd_retry, now_us,
                __atomic_load_n(&s_video_hdmi_connected, __ATOMIC_ACQUIRE));
            if (retry_action == HDMI_HPD_RETRY_GIVE_UP) {
                ESP_LOGI(TAG,
                         "HDMI recovery: no signal after %d pulses; no more "
                         "pulses until HPD rises again",
                         HDMI_HPD_RETRY_MAX_PULSES);
            } else if (retry_action == HDMI_HPD_RETRY_PULSE) {
                /* Low for TC_HPD_LOW_MIN_MS like any other HPD pulse of the
                 * Mini: the host display policy raises it again. */
                uint8_t sys_status = 0;
                (void)tc_get_sys_status(&ctx.bridge, &sys_status);
                ESP_LOGI(TAG,
                         "HDMI recovery pulse %" PRIu32 ": +5 V and no signal "
                         "for %" PRId64 " s since HPD rose (sys=0x%02x tmds=%d "
                         "sync=%d); HPD low for %d ms",
                         hpd_retry.pulses, (now_us - wait_start_us) / 1000000,
                         sys_status, !!(sys_status & MASK_S_TMDS),
                         !!(sys_status & MASK_S_SYNC), TC_HPD_LOW_MIN_MS);
                if (tc_set_hpd_output(&ctx.bridge, false) == ESP_OK) {
                    s_host_display_hidden_since_us = esp_timer_get_time();
                    host_display_advertised = false;
                    hpd_retry_pulse_low = true;
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                ESP_LOGE(TAG, "HDMI recovery pulse: HPD low failed");
                (void)tc_set_hpd_output(&ctx.bridge, true);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        hdmi_hpd_retry_signal(&hpd_retry);
        no_signal_count = 0;
        loop_trace++;

        bool any_candidate = false;
        for (size_t i = 0; i < sizeof(s_candidates) / sizeof(s_candidates[0]); i++) {
            if (tc_candidate_supports_mode(&s_candidates[i], &detected)) {
                any_candidate = true;
                break;
            }
        }
        if (!expect_video) {
            hdmi_health_pause(&health);
        } else if (hdmi_health_check(&ctx, &health, &hpd_retry, &detected,
                                     any_candidate)) {
            continue;
        }
        if (!any_candidate) {
            ESP_LOGW(TAG, "unsupported HDMI mode %ux%u@%u frame=%ux%u for all candidates",
                     detected.width, detected.height, detected.fps, detected.frame_width, detected.frame_height);
            ESP_ERROR_CHECK(tc_enable_stream(&ctx.bridge, false));
            ESP_ERROR_CHECK(csi_destroy(&ctx));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!ctx.cam || !timings_equal(&ctx.active_timings, &detected)) {
            ESP_LOGI(TAG, "new HDMI mode %ux%u@%u", detected.width, detected.height, detected.fps);
            tc_log_signal_status(&ctx.bridge);
            ret = tc_try_candidates(&ctx, &detected);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "all candidates failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            continue;
        }

    }
}


esp_err_t jetkvm_video_reserve_buffers(void)
{
    if (s_video_pool.h264_db != NULL) {
        return ESP_OK;
    }
    size_t alignment = 0;
    uint32_t db_bytes = 0;
    esp_err_t ret = esp_cache_get_alignment(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &alignment);
    if (ret != ESP_OK) {
        return ret;
    }
    /* DMA-capable PSRAM. Not zeroed: CSI overwrites each frame it hands on. */
    for (size_t i = 0; i < VIDEO_POOL_CAPTURE_SLOTS; i++) {
        s_video_pool.capture[i] = heap_caps_aligned_alloc(
            alignment, VIDEO_POOL_CAPTURE_SLOT_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (s_video_pool.capture[i] == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto fail;
        }
    }
    /* The DB of the largest encode resolution serves every smaller mode. */
    s_video_pool.h264_db = esp_h264_aligned_malloc(
        64, 1, (uint32_t)h264_hw_database_bytes(FRAME_WIDTH_MAX, FRAME_HEIGHT_MAX),
        &db_bytes, ESP_H264_MEM_SPIRAM);
    if (s_video_pool.h264_db == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_video_pool.h264_db_bytes = db_bytes;
    if (esp_h264_enc_hw_set_db_pool(s_video_pool.h264_db, db_bytes) !=
        ESP_H264_ERR_OK) {
        ret = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    ESP_LOGI(TAG, "video pool: %u x %u B capture + %" PRIu32 " B H.264 DB; "
             "PSRAM free=%u largest=%u",
             (unsigned)VIDEO_POOL_CAPTURE_SLOTS,
             (unsigned)VIDEO_POOL_CAPTURE_SLOT_BYTES, db_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;

fail:
    heap_caps_free(s_video_pool.h264_db);
    for (size_t i = 0; i < VIDEO_POOL_CAPTURE_SLOTS; i++) {
        heap_caps_free(s_video_pool.capture[i]);
    }
    memset(&s_video_pool, 0, sizeof(s_video_pool));
    ESP_LOGE(TAG, "video pool allocation failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t jetkvm_video_start(void)
{
    BaseType_t created = xTaskCreatePinnedToCore(
        hdmi_capture_task, "hdmi_capture", HDMI_CAPTURE_TASK_STACK, NULL,
        tskIDLE_PRIORITY + 2, NULL, VIDEO_MEDIA_CORE);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool jetkvm_video_is_hdmi_connected(void)
{
    return __atomic_load_n(&s_video_hdmi_connected, __ATOMIC_ACQUIRE);
}

bool jetkvm_video_is_sleep_active(void)
{
    return __atomic_load_n(&s_video_sleep_active, __ATOMIC_ACQUIRE);
}

void jetkvm_video_set_network_available(bool available)
{
    __atomic_store_n(&s_network_available, available, __ATOMIC_RELEASE);
}

void jetkvm_video_set_host_display_idle_enabled(bool enabled)
{
    __atomic_store_n(&s_host_display_idle_enabled, enabled, __ATOMIC_RELEASE);
}

void jetkvm_video_set_sleep_after_seconds(int32_t duration_seconds)
{
    __atomic_store_n(&s_video_sleep_after_seconds, duration_seconds,
                     __ATOMIC_RELEASE);
}

void jetkvm_video_set_capture_suspended(bool suspended)
{
    __atomic_store_n(&s_capture_suspended, suspended, __ATOMIC_RELEASE);
}

bool jetkvm_video_capture_suspended(void)
{
    return __atomic_load_n(&s_capture_suspended, __ATOMIC_ACQUIRE);
}

esp_err_t jetkvm_video_present_guest(const uint8_t edid[256])
{
    static const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0x00};
    if (!edid || memcmp(edid, header, sizeof(header)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t sums[2] = {0, 0};
    for (size_t i = 0; i < 256U; i++) {
        sums[i / 128U] = (uint8_t)(sums[i / 128U] + edid[i]);
    }
    if (sums[0] != 0U || sums[1] != 0U) {
        return ESP_ERR_INVALID_CRC;
    }
    guest_display_queue(GUEST_DISPLAY_REQUEST_PRESENT, edid);
    return ESP_OK;
}

esp_err_t jetkvm_video_restore_physical(void)
{
    guest_display_queue(GUEST_DISPLAY_REQUEST_RESTORE, NULL);
    return ESP_OK;
}

bool jetkvm_video_guest_owns_display(void)
{
    return __atomic_load_n(&s_guest_display.owns, __ATOMIC_ACQUIRE);
}

bool jetkvm_video_guest_display_presented(void)
{
    return __atomic_load_n(&s_guest_display.presented, __ATOMIC_ACQUIRE);
}

esp_err_t jetkvm_video_lend_guest_buffers(uint8_t *buffers[], size_t count,
                                          size_t *bytes)
{
    if (count > VIDEO_POOL_CAPTURE_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_guest_display_mux);
    /* Presented: the capture task tore capture down before the EDID went
     * out, and only a restore brings it back, which the loan holds off. */
    if (s_guest_display.presented && !s_guest_buffers_lent) {
        for (size_t i = 0; i < count; i++) {
            buffers[i] = s_video_pool.capture[i];
        }
        *bytes = VIDEO_POOL_CAPTURE_SLOT_BYTES;
        __atomic_store_n(&s_guest_buffers_lent, true, __ATOMIC_RELEASE);
        ret = ESP_OK;
    }
    portEXIT_CRITICAL(&s_guest_display_mux);
    return ret;
}

void jetkvm_video_return_guest_buffers(void)
{
    __atomic_store_n(&s_guest_buffers_lent, false, __ATOMIC_RELEASE);
}
