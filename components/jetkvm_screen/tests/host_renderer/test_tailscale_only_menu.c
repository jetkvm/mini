/*
 * Advanced > Tailscale Only on the Mini's screen, with the production
 * screens and the production button code: the entry is on the page only
 * while the setting is on, a held bottom button on it asks to turn the
 * setting off, and its text fits beside the button strip at each edge.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "host_ui.h"
#include "jetkvm_screen_button.h"
#include "jetkvm_screen_buttons.h"
#include "jetkvm_screen_tailscale_only.h"
#include "screens.h"
#include "ui.h"

#define BUTTON_DOWN 1U

static int s_failures;
static unsigned s_calls;
static char s_method[40], s_params[40];

static void rpc(const char *method, const char *params)
{
    ++s_calls;
    snprintf(s_method, sizeof(s_method), "%s", method);
    snprintf(s_params, sizeof(s_params), "%s", params != NULL ? params : "");
}

static void expect(bool condition, const char *what)
{
    if (!condition) {
        ++s_failures;
        fprintf(stderr, "Tailscale Only menu: %s\n", what);
    }
}

static void open_advanced(void)
{
    /* Opening the page puts the highlight on its first entry. */
    loadScreen(SCREEN_ID_MENU_SCREEN);
    loadScreen(SCREEN_ID_MENU_ADVANCED_SCREEN);
    lv_obj_update_layout(objects.menu_advanced_screen);
}

static void set(bool enabled)
{
    /* As apply_state() of jetkvm_screen.c does with each state. */
    if (jetkvm_screen_tailscale_only_apply(enabled)) {
        jetkvm_screen_buttons_set_tailscale_only(enabled);
    }
}

static bool focused(lv_obj_t *entry)
{
    return lv_obj_has_state(entry, LV_STATE_FOCUSED);
}

int main(void)
{
    if (!host_ui_start()) {
        fprintf(stderr, "cannot create LVGL display\n");
        return 1;
    }
    host_ui_start_buttons();
    ui_set_rpc_handler(rpc);
    lv_obj_t *entry = jetkvm_screen_tailscale_only_entry();

    /* Setting off: no entry, and the page starts at Reboot Device. */
    set(false);
    open_advanced();
    expect(lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN), "the entry is on the page while the setting is off");
    expect(focused(objects.menu_btn_advanced_reboot) && !focused(entry),
           "the page does not start at Reboot Device while the setting is off");

    /* Setting on while the page is open: the entry appears first and takes
     * the highlight; one tap down goes to Reboot Device. */
    set(true);
    lv_obj_update_layout(objects.menu_advanced_screen);
    expect(!lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN), "the entry is not on the page while the setting is on");
    expect(focused(entry) && !focused(objects.menu_btn_advanced_reboot),
           "the entry does not take the highlight when it appears");
    lv_area_t first, second;
    lv_obj_get_coords(entry, &first);
    lv_obj_get_coords(objects.menu_btn_advanced_reboot, &second);
    expect(first.y2 < second.y1, "the entry is not above Reboot Device");
    host_ui_tap(BUTTON_DOWN);
    expect(focused(objects.menu_btn_advanced_reboot) && !focused(entry),
           "one tap down from the entry does not reach Reboot Device");

    /* Its text stays inside the entry beside the strip at each edge. */
    static const uint16_t turns[] = {0, 90, 180, 270, 0};
    for (size_t i = 0; i < sizeof(turns) / sizeof(turns[0]); ++i) {
        jetkvm_screen_buttons_set_rotation(turns[i]);
        open_advanced();
        lv_obj_t *label = lv_obj_get_child(entry, 0);
        const char *text = lv_label_get_text(label);
        lv_area_t box, inside;
        lv_obj_get_coords(label, &box);
        lv_obj_get_content_coords(entry, &inside);
        int32_t width = lv_text_get_width(text, (uint32_t)strlen(text),
                                          lv_obj_get_style_text_font(label, LV_PART_MAIN), 0);
        if (width > lv_area_get_width(&box) || box.x1 < inside.x1 || box.x2 > inside.x2) {
            ++s_failures;
            fprintf(stderr,
                    "Tailscale Only menu: \"%s\" does not fit its entry at %u degrees "
                    "(text %d px, label x %d to %d, entry x %d to %d)\n",
                    text, (unsigned)turns[i], (int)width, (int)box.x1, (int)box.x2, (int)inside.x1,
                    (int)inside.x2);
        }
    }

    /* A tap on the entry does nothing: it steps down. A hold that is
     * shorter than the accept time does nothing either. */
    open_advanced();
    s_calls = 0;
    host_ui_hold(BUTTON_DOWN, JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS / 2U);
    expect(s_calls == 0, "a short press on the entry called the device");

    /* The hold on the entry asks to turn the setting off, once. */
    open_advanced();
    expect(focused(entry), "the page does not start at the entry while the setting is on");
    s_calls = 0;
    host_ui_hold(BUTTON_DOWN, JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 300U);
    expect(s_calls == 1 && strcmp(s_method, "setTailscaleOnlyAccess") == 0 &&
               strcmp(s_params, "{\"enabled\":false}") == 0,
           "the hold on the entry did not ask once for setTailscaleOnlyAccess with enabled false");

    /* The next state has the setting off: the entry goes, and the same hold
     * is then the hold on Reboot Device. */
    set(false);
    lv_obj_update_layout(objects.menu_advanced_screen);
    expect(lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN), "the entry stays on the page after the setting went off");
    expect(focused(objects.menu_btn_advanced_reboot),
           "the highlight is not on Reboot Device after the entry went");
    s_calls = 0;
    host_ui_hold(BUTTON_DOWN, JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 300U);
    expect(s_calls == 1 && strcmp(s_method, "reboot") == 0,
           "the hold on the first entry is not Reboot Device after the entry went");

    /* The entry goes while the button is held on it (another session turned
     * the setting off): the hold must not go on as a hold on Reboot Device,
     * which takes its place, and the release is not a tap. */
    set(true);
    open_advanced();
    s_calls = 0;
    host_ui_press(BUTTON_DOWN);
    host_ui_keep(JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS / 2U);
    set(false);
    host_ui_keep(JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 300U);
    host_ui_release(BUTTON_DOWN);
    expect(s_calls == 0, "a hold that began on the entry acted on the entry that took its place");
    expect(focused(objects.menu_btn_advanced_reboot), "the release of that hold moved the highlight");
    /* The same when the entry appears under a hold on Reboot Device. */
    host_ui_press(BUTTON_DOWN);
    host_ui_keep(JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS / 2U);
    set(true);
    host_ui_keep(JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 300U);
    host_ui_release(BUTTON_DOWN);
    expect(s_calls == 0, "a hold that began on Reboot Device acted on the entry that appeared");
    expect(focused(entry), "the highlight is not on the entry that appeared");
    set(false);

    if (s_failures != 0) {
        fprintf(stderr, "Tailscale Only menu: %d failures\n", s_failures);
        return 1;
    }
    puts("Tailscale Only menu entry: shown only while on, first on the page, hold turns it off: OK");
    return 0;
}
