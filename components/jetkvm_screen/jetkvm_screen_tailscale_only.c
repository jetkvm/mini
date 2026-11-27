#include "jetkvm_screen_tailscale_only.h"

#include <stddef.h>

#include "ui/fonts.h"
#include "ui/screens.h"
#include "ui/styles.h"
#include "ui/ui.h"

static lv_obj_t *s_entry;
static bool s_shown;

static void clicked(lv_event_t *event)
{
    (void)event;
    ui_call_rpc_handler("setTailscaleOnlyAccess", "{\"enabled\":false}");
}

void jetkvm_screen_tailscale_only_init(void)
{
    /* A menu entry as the generated ones, first in the Advanced column. */
    s_entry = lv_button_create(lv_obj_get_parent(objects.menu_btn_advanced_reboot));
    lv_obj_set_size(s_entry, LV_PCT(100), 50);
    lv_obj_remove_flag(s_entry, LV_OBJ_FLAG_SNAPPABLE);
    add_style_menu_button(s_entry);
    lv_obj_add_event_cb(s_entry, clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(s_entry);
    add_style_menu_button_label(label);
    /* As "USB Debugging: Off": the generated 20 px face does not fit beside
     * a vertical button strip. */
    lv_obj_set_style_text_font(label, &ui_font_font_book18, LV_PART_MAIN);
    lv_label_set_text(label, "Tailscale Only: On");
    lv_obj_move_to_index(s_entry, 0);
    lv_obj_add_flag(s_entry, LV_OBJ_FLAG_HIDDEN);
}

bool jetkvm_screen_tailscale_only_apply(bool enabled)
{
    if (s_entry == NULL || enabled == s_shown) {
        return false;
    }
    s_shown = enabled;
    if (enabled) {
        lv_obj_remove_flag(s_entry, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_entry, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

lv_obj_t *jetkvm_screen_tailscale_only_entry(void)
{
    return s_entry;
}
