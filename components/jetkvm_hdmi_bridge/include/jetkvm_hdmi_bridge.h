/* TC358743 HDMI -> MIPI-CSI bridge driver for the JetKVM P4 firmware.
 *
 * Everything the driver needs is carried by the jetkvm_hdmi_bridge_t
 * context that main embeds into its own capture context.
 *
 * Concurrency contract:
 *  - Only the capture-owner task calls tc_* driver entry points and
 *    p4_edid_control_process().  RPC/HTTP/WebRTC tasks reach the bridge only
 *    through the p4_edid_control_* mailbox below.
 *  - p4_edid_control_process() must keep running on the capture-owner task,
 *    which rebuilds capture after an apply; setEDID only queues the command
 *    and answers without waiting for the hardware.
 *
 * There is no tests/ directory: every entry point programs real
 * TC358743/CSI registers over I2C and cannot run on the host. */

#ifndef JETKVM_HDMI_BRIDGE_H
#define JETKVM_HDMI_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "esp_cam_ctlr_csi.h"
#include "jetkvm_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bps_per_lane;
    uint32_t lineinitcnt;
    uint32_t lptxtimecnt;
    uint32_t tclk_headercnt;
    uint32_t tclk_trailcnt;
    uint32_t ths_headercnt;
    uint32_t twakeup;
    uint32_t tclk_postcnt;
    uint32_t ths_trailcnt;
    uint32_t hstxvregcnt;
} tc358743_csi_preset_t;

typedef struct {
    uint32_t target_bps_per_lane;
    const tc358743_csi_preset_t *timing_preset;
} tc358743_link_profile_t;

typedef struct {
    const char *name;
    uint8_t data_lane_num;
    bool output_rgb888;
    bool byte_swap;
    bool use_colorbar;
    bool use_full_timing;
    bool use_extcnt;
    bool bridge_has_hsync;
    bool disable_auto_mute;
    bool continuous_clock;
    bool hsckmd;
    bool eot_disable;
    mipi_csi_phy_clock_source_t clk_src;
    uint8_t hs_freq_sel_override;
    uint8_t dtype_min;
    uint8_t dtype_max;
    uint32_t lane_bps;
    const tc358743_csi_preset_t *timing_preset;
} tc358743_capture_candidate_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t frame_width;
    uint16_t frame_height;
    uint16_t frame_interval_x10ms;
    uint16_t fps;
    bool interlaced;
} tc358743_timings_t;

typedef struct {
    jetkvm_video_edid_preset_t preset;
    const uint8_t *bytes;
    size_t size;
    const char *label;
} p4_edid_descriptor_t;

/* Everything the bridge driver touches, and nothing else.  main embeds this
 * into app_ctx_t and keeps ownership of every field:
 *  - i2c_bus:         created by tc_i2c_init(), used by all register access.
 *  - isp:             owned by main's CSI pipeline; diagnostics only null-check
 *                     it to decide whether the ISP dump is meaningful.
 *  - refclk_hz:       written by tc_initial_setup(), read back by main's
 *                     capture orchestrator when it programs the PLL.
 *  - active_candidate:owned by main's candidate state machine; tc_enable_stream
 *                     and tc_set_csi read it for lane/mute/clock policy.
 *  - log_tag:         ESP_LOG tag for every driver log line ("hdmi_csi"). */
typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    isp_proc_handle_t isp;
    uint32_t refclk_hz;
    const tc358743_capture_candidate_t *active_candidate;
    const char *log_tag;
} jetkvm_hdmi_bridge_t;

/* Board bring-up. */
void tc_gpio_init(void);
void tc_hw_reset(jetkvm_hdmi_bridge_t *ctx);
void tc_i2c_init(jetkvm_hdmi_bridge_t *ctx);
esp_err_t tc_probe_chip(jetkvm_hdmi_bridge_t *ctx);

/* Low-level register access.  Only tc_write_reg is used outside the
 * component (by main's capture orchestrator); the rest of the family stays
 * internal. */
esp_err_t tc_write_reg(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, int size,
                       uint32_t value);

/* Receiver configuration helpers used by main's capture orchestrator. */
esp_err_t tc_reset_phy(jetkvm_hdmi_bridge_t *ctx);
esp_err_t tc_set_pll(jetkvm_hdmi_bridge_t *ctx, uint32_t refclk_hz,
                     uint32_t bps_per_lane);
esp_err_t tc_set_csi(jetkvm_hdmi_bridge_t *ctx,
                     const tc358743_csi_preset_t *preset);
esp_err_t tc_set_yuv422_output(jetkvm_hdmi_bridge_t *ctx);
esp_err_t tc_set_colorbar_output(jetkvm_hdmi_bridge_t *ctx);
esp_err_t tc_set_rgb888_output(jetkvm_hdmi_bridge_t *ctx);
esp_err_t tc_enable_stream(jetkvm_hdmi_bridge_t *ctx, bool enable);
esp_err_t tc_set_hpd_output(jetkvm_hdmi_bridge_t *ctx, bool advertised);
/* Shortest time HPD stays low before it is raised again. Some sources ignore
 * a shorter pulse: a Mac powers its DP-to-HDMI converter off only 300 ms
 * after HPD falls, and a pulse shorter than that leaves the converter's
 * HDMI output stopped after HPD rises. */
#define TC_HPD_LOW_MIN_MS 2000

esp_err_t tc_initial_setup(jetkvm_hdmi_bridge_t *ctx, uint32_t refclk_hz,
                           const p4_edid_descriptor_t *edid);
/* Writes and verifies the EDID RAM with HPD low, holds HPD low for
 * TC_HPD_LOW_MIN_MS and raises it again, also after a failure. Leaves the
 * EDID mailbox's applied preset alone. The bytes form takes any two-block
 * EDID, such as the OS Services guest display's. */
esp_err_t tc_program_edid(jetkvm_hdmi_bridge_t *ctx,
                          const p4_edid_descriptor_t *descriptor);
esp_err_t tc_program_edid_bytes(jetkvm_hdmi_bridge_t *ctx,
                                const uint8_t edid[256]);

/* Signal detection. */
/* The receiver's SYS_STATUS register (the MASK_S_* bits of tc358743_regs.h),
 * independent of HPD and capture state: MASK_S_DDC5V is the source's +5 V,
 * MASK_S_TMDS its TMDS clock, MASK_S_PHY_PLL and MASK_S_PHY_SCDT the lock of
 * the receiver's PHY on it, MASK_S_SYNC the video sync. On an I2C error,
 * sys_status is left unchanged. Capture-owner task only. */
esp_err_t tc_get_sys_status(jetkvm_hdmi_bridge_t *ctx, uint8_t *sys_status);
esp_err_t tc_get_detected_timings(jetkvm_hdmi_bridge_t *ctx,
                                  tc358743_timings_t *timings);

/* Interrupt polling and diagnostic dumps (log only). */
void tc_poll_interrupts(jetkvm_hdmi_bridge_t *ctx);
void tc_log_signal_status(jetkvm_hdmi_bridge_t *ctx);
void tc_log_top_interrupts(jetkvm_hdmi_bridge_t *ctx);
void tc_log_sub_interrupts(jetkvm_hdmi_bridge_t *ctx);
void tc_log_csi_status(jetkvm_hdmi_bridge_t *ctx);
void tc_log_video_output(jetkvm_hdmi_bridge_t *ctx);

/* ESP32-P4 side CSI/ISP diagnostics and stop-state wait (log/poll only). */
void p4_log_csi_status(jetkvm_hdmi_bridge_t *ctx);
void p4_log_isp_status(const jetkvm_hdmi_bridge_t *ctx);
bool p4_wait_stopstate(jetkvm_hdmi_bridge_t *ctx, uint8_t lane_num,
                       uint32_t timeout_ms);

/* EDID preset table. */
const p4_edid_descriptor_t *p4_edid_descriptor(
    jetkvm_video_edid_preset_t preset);
const p4_edid_descriptor_t *p4_edid_presets(size_t *count_out);
bool p4_edid_hex_matches(const char *hex,
                         const p4_edid_descriptor_t *descriptor);
bool p4_edid_to_hex(const p4_edid_descriptor_t *descriptor,
                    char *output, size_t output_capacity);

/* EDID command mailbox.  The state is component-private static storage
 * because the RPC task must reach it independently of the capture owner's
 * (stack-allocated) bridge context, and getEDID is served before the owner
 * marks the mailbox ready. */
esp_err_t p4_edid_control_init(void);
bool p4_edid_control_lock(uint32_t timeout_ms);
void p4_edid_control_unlock(void);
bool p4_edid_control_ready(void);
/* True while a queued command waits for the capture owner. */
bool p4_edid_control_pending(void);
jetkvm_video_edid_preset_t p4_edid_control_applied_preset(void);
bool p4_edid_control_mark_ready(jetkvm_video_edid_preset_t preset);
/* Queue the command and return at once; the capture owner applies it and
 * logs the outcome.  For callers that must not block their task, such as the
 * JSON-RPC dispatcher, which shares the WebRTC peer loop. */
esp_err_t p4_edid_submit_async_locked(jetkvm_video_edid_preset_t preset);

/* Outcome of a queued apply, reported from the capture owner after any
 * hardware rollback; `applied` is the preset the bridge uses now. The
 * handler must not block the owner. */
typedef void (*p4_edid_apply_result_fn)(jetkvm_video_edid_preset_t requested,
                                        jetkvm_video_edid_preset_t applied,
                                        esp_err_t error, void *ctx);
void p4_edid_set_apply_result_handler(p4_edid_apply_result_fn handler, void *ctx);

/* Consume at most one EDID command.  Capture-owner task only — see the
 * concurrency contract at the top of this header. */
bool p4_edid_control_process(jetkvm_hdmi_bridge_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* JETKVM_HDMI_BRIDGE_H */
