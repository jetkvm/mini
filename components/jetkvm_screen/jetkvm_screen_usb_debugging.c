#include "jetkvm_screen_usb_debugging.h"

#include <stddef.h>

#include "lvgl.h"
#include "ui/fonts.h"
#include "ui/screens.h"
#include "ui/styles.h"
#include "ui/ui.h"

/*
 * The generated USB Emulation entry has no action and the Mini hides it, so
 * it carries the setting: it moves below Reset Config, and the restart note
 * follows it in the same column. The note uses the web UI's wording.
 */

static lv_obj_t *s_button, *s_label, *s_note;
static bool s_applied, s_enabled, s_active;

static void clicked(lv_event_t *event)
{
    (void)event;
    ui_call_rpc_handler("setUsbDebuggingState",
                        s_enabled ? "{\"enabled\":false}" : "{\"enabled\":true}");
}

void jetkvm_screen_usb_debugging_init(void)
{
    s_button = objects.menu_btn_advanced_usb_emulation;
    s_label = lv_obj_get_child(s_button, 0);
    lv_obj_move_foreground(s_button);
    lv_obj_remove_flag(s_button, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_add_event_cb(s_button, clicked, LV_EVENT_CLICKED, NULL);
    /* "USB Debugging: Off" does not fit beside a vertical strip in the
     * generated 20 px face. */
    lv_obj_set_style_text_font(s_label, &ui_font_font_book18, LV_PART_MAIN);
    lv_label_set_text(s_label, "USB Debugging: Off");

    s_note = lv_label_create(lv_obj_get_parent(s_button));
    add_style_label_font16(s_note);
    lv_obj_set_style_text_color(s_note, lv_color_hex(0x94a3b8), LV_PART_MAIN);
    lv_obj_set_width(s_note, LV_PCT(100));
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_note, "Takes effect at the next restart.");
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}

void jetkvm_screen_usb_debugging_apply(bool enabled, bool active)
{
    if (s_button == NULL || (s_applied && enabled == s_enabled && active == s_active)) {
        return;
    }
    s_applied = true;
    s_enabled = enabled;
    s_active = active;
    lv_label_set_text(s_label, enabled ? "USB Debugging: On" : "USB Debugging: Off");
    if (enabled == active) {
        lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    /* The entry is the last in its scrolling column; keep the note that
     * appears below the highlighted entry on the page. */
    if (lv_obj_has_state(s_button, LV_STATE_FOCUSED)) {
        lv_obj_update_layout(s_note);
        lv_obj_scroll_to_view_recursive(s_note, LV_ANIM_OFF);
    }
}
