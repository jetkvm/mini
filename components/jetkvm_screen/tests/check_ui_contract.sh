#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_dir="$(cd "$component_dir/../.." && pwd)"

(
    cd "$component_dir"
    sha256sum --check ui.sha256
)

upstream="$repo_dir/../kvm/internal/native/eez/src/ui"
if [[ -d "$upstream" ]]; then
    diff -qr --exclude=screens.c --exclude=styles.c "$upstream" "$component_dir/ui"
fi

grep -q 'JETKVM_SCREEN_SOURCE_WIDTH 240U' "$component_dir/jetkvm_screen_fit.h"
grep -q 'JETKVM_SCREEN_PANEL_NATIVE_HEIGHT 240U' "$component_dir/jetkvm_screen_fit.h"
grep -q 'JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH 240U' "$component_dir/jetkvm_screen_fit.h"
! grep -q 'JETKVM_SCREEN_ROTATION_270_SOURCE_X_OFFSET' "$component_dir/jetkvm_screen_fit.h"
grep -q 'JETKVM_SCREEN_FLUSH_BUFFER_PIXELS' "$component_dir/jetkvm_screen_fit.h"
grep -q 'LCD_RGB_ELEMENT_ORDER_RGB' "$component_dir/jetkvm_screen.c"
grep -q 'LCD_RGB_DATA_ENDIAN_BIG' "$component_dir/jetkvm_screen.c"
grep -q '\.miso_io_num = -1' "$component_dir/jetkvm_screen.c"
grep -q 'default JETKVM_SCREEN_ROTATION_0' "$component_dir/Kconfig"
grep -q 'config JETKVM_SCREEN_ROTATION_90' "$component_dir/Kconfig"
grep -q 'config JETKVM_SCREEN_ROTATION_180' "$component_dir/Kconfig"
grep -q 'config JETKVM_SCREEN_ROTATION_270' "$component_dir/Kconfig"
grep -q 'SCREEN_PANEL_X_GAP' "$component_dir/jetkvm_screen.c"
grep -q 'SCREEN_PANEL_Y_GAP' "$component_dir/jetkvm_screen.c"
grep -q 'SCREEN_PANEL_SWAP_XY' "$component_dir/jetkvm_screen.c"
grep -q 'SCREEN_PANEL_MIRROR_X' "$component_dir/jetkvm_screen.c"
grep -q 'SCREEN_PANEL_MIRROR_Y' "$component_dir/jetkvm_screen.c"
grep -q 'jetkvm_screen_fit_copy_rgb565_be' "$component_dir/jetkvm_screen.c"
test "$(grep -c 'lv_obj_set_size(obj, 240, 240)' "$component_dir/ui/screens.c")" -eq 12
grep -q 'lv_style_set_pad_top(style, 16)' "$component_dir/ui/styles.c"
grep -q 'lv_style_set_pad_bottom(style, 16)' "$component_dir/ui/styles.c"
grep -q 'lv_style_set_pad_left(style, 12)' "$component_dir/ui/styles.c"
grep -q 'lv_style_set_pad_right(style, 12)' "$component_dir/ui/styles.c"
grep -q 'lv_obj_set_style_pad_row(obj, 6' "$component_dir/ui/screens.c"
grep -q 'lv_image_set_src(obj, &img_setup_browser)' "$component_dir/ui/screens.c"
grep -q 'lv_label_set_text(obj, "Set up Wi-Fi")' "$component_dir/ui/screens.c"
grep -q 'lv_label_set_text(obj, "Visit jetkvm.com/wireless")' "$component_dir/ui/screens.c"
grep -q 'lv_label_set_text(obj, "Connect Ethernet cable")' "$component_dir/ui/screens.c"
test -f "$component_dir/assets/setup_browser.png"
grep -q 'set_connection_label(objects.usb_status_label, state->usb_connected)' "$component_dir/jetkvm_screen.c"
grep -q 'set_wifi_status(true, state->wifi_ssid, state->wifi_signal_valid' "$component_dir/jetkvm_screen.c"
grep -q 'lv_label_set_text(s_wifi_signal_icon, LV_SYMBOL_WIFI)' "$component_dir/jetkvm_screen.c"
! grep -q 'dBm' "$component_dir/jetkvm_screen.c"
grep -q 'xTaskCreatePinnedToCore' "$component_dir/jetkvm_screen.c"
grep -q 'SCREEN_TASK_CORE 0' "$component_dir/jetkvm_screen.c"
grep -q 'if (wait_ticks < 1U)' "$component_dir/jetkvm_screen.c"
grep -q 'set_visible(objects.menu_btn_network, JETKVM_BOARD_WIFI)' "$component_dir/jetkvm_screen.c"
grep -q 'set_visible(objects.menu_btn_access, false)' "$component_dir/jetkvm_screen.c"
grep -q 'rendered != JETKVM_SCREEN_PAGE_BOOT' "$component_dir/jetkvm_screen.c"
grep -q 'lv_refr_now(screen->display)' "$component_dir/jetkvm_screen.c"
grep -q 'SCREEN_FIRST_REFRESH_TIMEOUT_MS' "$component_dir/jetkvm_screen.c"
grep -q 'lv_display_set_flush_wait_cb(screen->display, screen_flush_wait)' "$component_dir/jetkvm_screen.c"
grep -q 'xSemaphoreTake(screen->flush_done, portMAX_DELAY)' "$component_dir/jetkvm_screen.c"
! grep -q 'SCREEN_CENTERED_PAGE_TRANSLATE_X' "$component_dir/jetkvm_screen.c"

echo "jetkvm_screen UI and hardware contracts passed"
