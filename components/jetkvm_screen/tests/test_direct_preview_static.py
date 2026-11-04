#!/usr/bin/env python3
"""Guard the PPA-to-ST7789 direct-preview ownership contract."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[1]
ROOT = COMPONENT.parents[1]
SOURCE = (COMPONENT / "jetkvm_screen.c").read_text()
HEADER = (COMPONENT / "include" / "jetkvm_screen.h").read_text()
VIDEO = (ROOT / "components" / "jetkvm_video" / "jetkvm_video.c").read_text()


def require(fragment: str, text: str = SOURCE) -> None:
    assert fragment in text, f"missing direct-preview contract: {fragment}"


require("#define SCREEN_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)")
require("#define SCREEN_PREVIEW_BUFFER_COUNT 2U")
require("uint16_t *preview_buffers[SCREEN_PREVIEW_BUFFER_COUNT]")
require("MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA")
require(".flags.psram_dma_direct = true")
require("PPA_SRM_COLOR_MODE_YUV420")
require("PPA_SRM_COLOR_MODE_YUV422_UYVY")
require("jetkvm_screen_preview_submit_uyvy", HEADER)
require(".srm_cm = PPA_SRM_COLOR_MODE_RGB565")
require("PPA_TRANS_MODE_BLOCKING")
require("screen_preview_draw_osd(output_buffer, width, height, fps)")
require('screen_preview_draw_text_box(buffer, "NO NETWORK", 5U)')
require("normalized.network_connected, __ATOMIC_RELEASE")
require("bool partial_refresh = !layout_changed && partial_capable")
require("!s_screen.preview_layout_valid")
require("transfer_y * SCREEN_PREVIEW_WIDTH")
require("screen_preview_swap_rgb565_bytes(transfer_buffer, transfer_pixels)")
require("SCREEN_TRANSFER_LVGL")
require("SCREEN_TRANSFER_PREVIEW")
require("esp_lcd_panel_draw_bitmap(s_screen.panel, 0, transfer_y,")
require("transfer_y + transfer_height")
require("screen_transfer_acquire(&s_screen")

# The former LVGL image overlay serialized render and SPI work and limited the
# complete refresh rate. HDMI preview must remain outside that object graph.
for old_lvgl_overlay in (
    "preview_root",
    "lv_image_dsc_t preview_image",
    "screen->preview_osd",
    "lv_image_cache_drop",
):
    assert old_lvgl_overlay not in SOURCE, old_lvgl_overlay

for stat in (
    "preview_ppa_frames",
    "preview_ppa_total_us",
    "preview_ppa_max_us",
):
    require(stat, HEADER)
    require(stat, VIDEO)
require("ppa_capacity_fps=", VIDEO)

print("direct PPA-to-ST7789 preview contract: ok")
