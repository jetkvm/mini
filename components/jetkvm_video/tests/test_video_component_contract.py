#!/usr/bin/env python3
"""Guard the concurrency and ownership boundaries of jetkvm_video."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[1]
ROOT = COMPONENT.parents[1]
SOURCE = (COMPONENT / "jetkvm_video.c").read_text()
HEADER = (COMPONENT / "include" / "jetkvm_video.h").read_text()
MAIN = (ROOT / "main" / "main.c").read_text()
BOARD_NETWORK = (ROOT / "components" / "jetkvm_board_network" /
                 "jetkvm_board_network.c").read_text()
BRIDGE = (ROOT / "components" / "jetkvm_hdmi_bridge" /
          "jetkvm_hdmi_bridge.c").read_text()
BRIDGE_REGS = (ROOT / "components" / "jetkvm_hdmi_bridge" /
               "tc358743_regs.h").read_text()


def require(fragment: str, text: str = SOURCE) -> None:
    assert fragment in text, f"missing contract fragment: {fragment}"


# ISR callbacks, their state and both locks must remain in one translation
# unit. Moving a lock behind extern would change interrupt/critical semantics.
require("static portMUX_TYPE s_csi_buffer_mux")
require("static portMUX_TYPE s_direct_h264_pipeline_mux")
require("static bool IRAM_ATTR csi_get_new_trans")
require("static bool IRAM_ATTR csi_trans_started")
require("static bool IRAM_ATTR csi_trans_finished")
require("static bool IRAM_ATTR direct_h264_pipeline_start_from_isr")
assert "extern portMUX_TYPE" not in SOURCE

# The capture-owner task is the only consumer of the EDID mailbox. The RPC
# side may submit and wait, but must never process the hardware command.
capture_start = SOURCE.index("static void hdmi_capture_task")
capture_end = SOURCE.index("esp_err_t jetkvm_video_start", capture_start)
capture = SOURCE[capture_start:capture_end]
require("p4_edid_control_process(&ctx.bridge)", capture)
assert SOURCE.count("p4_edid_control_process(") == 2  # primary + rollback service

# Keep private capture state out of the public API and the composition root.
assert "app_ctx_t" not in HEADER
assert "app_ctx_t" not in MAIN
assert "csi_get_new_trans" not in MAIN
assert "s_csi_buffer_mux" not in MAIN
assert "hdmi_capture_task" not in MAIN
require("ESP_ERROR_CHECK(jetkvm_video_start());", MAIN)

# On the current board VDD_O3 powers both the P4 MIPI D-PHY and the external
# TC358743 reference oscillator. RESETN must remain asserted until LDO3 is on;
# I2C cannot acknowledge before that oscillator is running.
assert "#define JETKVM_MIPI_LDO_CHAN_ID     3" in BRIDGE_REGS
assert "#define JETKVM_MIPI_LDO_VOLTAGE_MV 2500" in BRIDGE_REGS
gpio_init_start = BRIDGE.index("void tc_gpio_init(void)")
gpio_init_end = BRIDGE.index("void tc_hw_reset", gpio_init_start)
gpio_init = BRIDGE[gpio_init_start:gpio_init_end]
assert gpio_init.index("gpio_set_level(TC358743_RST, 0)") < gpio_init.index(
    "esp_ldo_acquire_channel")
capture_init = SOURCE[SOURCE.index('ESP_LOGI(TAG, "init:gpio")'):
                      SOURCE.index('ESP_LOGI(TAG, "init:done")')]
assert capture_init.index("tc_gpio_init();") < capture_init.index(
    "tc_hw_reset(&ctx.bridge);") < capture_init.index(
        "tc_i2c_init(&ctx.bridge);") < capture_init.index(
            "tc_probe_chip(&ctx.bridge)")

# Never propagate a partially locked HDMI detector result into CSI/H.264
# allocation. The observed bad-input diagnostics must retain the raw timing
# window so field failures can be distinguished from an I2C failure.
assert "timings->height < 480" in BRIDGE
assert '"unstable timing sys=0x%02x de=%ux%u frame=%ux%u fps=%u raw="' in BRIDGE

# Policy values remain atomic because settings/RPC and capture run on separate
# tasks. The public setters are the only main-to-video mutation boundary.
require("__atomic_store_n(&s_host_display_idle_enabled")
require("__atomic_store_n(&s_video_sleep_after_seconds")
require("__atomic_load_n(&s_video_sleep_active")
require("jetkvm_video_set_host_display_idle_enabled", MAIN)
require("jetkvm_video_set_sleep_after_seconds", MAIN)
require("void jetkvm_video_set_network_available(bool available)", HEADER)
require("jetkvm_video_set_network_available(link_up);", BOARD_NETWORK)
require("manual_preview || !__atomic_load_n(")
require("&s_network_available, __ATOMIC_ACQUIRE);")

# With Ethernet link available, boot into H.264 capture mode so the local
# display retains the generated KVM UI. After an encode, the source must go
# directly back to CSI; the encoded branch must never submit it to PPA.
require("#define VIDEO_MODE_DEFAULT_PREVIEW_ONLY 0")
# Reverse the IDI32 byte order and its declared YUV422 packing together before
# CSI_CM converts to encoder-native YUV420. Scale the GOP with the frame rate as
# JetKVM native/cgo does: two keyframes per second, one on the Wi-Fi board; all-IDR
# creates bursty RTP traffic and is not a cadence fix.
require("#define TC_CSI_INPUT_YUV422_COLOR  CAM_CTLR_COLOR_YUV422_YVYU")
require("#define TC_CSI_OUTPUT_YUV_COLOR    CAM_CTLR_COLOR_YUV420")
require("#define TC_CSI_INPUT_8BIT_SWAP     1")
require("#define TC_CSI_OUTPUT_BYTE_SWAP    0")
require("#define TC_H264_RAW_FORMAT         ESP_H264_RAW_FMT_O_UYY_E_VYY")
require("TC_CSI_OUTPUT_BYTE_SWAP,")
require("#define DIRECT_H264_OVERLAP_LEAD_US 3000")
require("#define H264_1080P_TARGET_BITRATE_BPS 7000000U")
require("#define H264_TARGET_BITRATE_MAX_BPS   12000000U")
require("#define H264_QUALITY_QP_MIN           28U")
require(".qp_min = H264_QUALITY_QP_MIN,")
require("configured_fps / H264_KEYFRAMES_PER_SECOND : 30;")
require("#define H264_KEYFRAMES_PER_SECOND 1U")
require("#define H264_KEYFRAMES_PER_SECOND 2U")
require("if (configured_gop < 1)")
assert "intra_only_uyvy_1080p30" not in SOURCE
encode_start = SOURCE.index(
    "static void direct_h264_pipeline_encode_task(void *arg)\n{")
encode_end = SOURCE.index("static bool timings_equal", encode_start)
encode_task = SOURCE[encode_start:encode_end]
encoded = encode_task[encode_task.index("h264_pipeline_encode_one("):]
assert "screen_preview_pipeline_submit(" not in encoded
assert encoded.index("h264_pipeline_encode_one(") < encoded.index(
    "csi_release_consumed_buffer(ctx, buffer_index)")
preview_start = SOURCE.index("static void screen_preview_task(void *arg)\n{")
preview_end = SOURCE.index(
    "static void video_mode_button_task(void *arg)\n{", preview_start)
preview_task = SOURCE[preview_start:preview_end]
assert preview_task.index("jetkvm_screen_preview_submit_ouyy_evyy(") < \
    preview_task.index("csi_release_consumed_buffer(ctx, "
                       "job.capture_buffer_index)")

print("jetkvm_video component contract: ok")
