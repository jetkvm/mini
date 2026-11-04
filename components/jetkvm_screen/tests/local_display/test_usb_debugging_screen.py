#!/usr/bin/env python3
"""Advanced > USB Debugging: the entry follows Reset Config, its label
follows the stored setting, the restart note shows while the setting and the
port differ, and the menu hold asks for setUsbDebuggingState with the other
value.

Compiles jetkvm_screen_usb_debugging.c, the Advanced entries of changed() and
the production button press, hold and release handlers of
jetkvm_screen_buttons.c against stub LVGL objects.
"""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[2]
buttons = (component / 'jetkvm_screen_buttons.c').read_text()


def cut(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first:source.index(end, first)]


handlers = cut(buttons, 'static void buttons_press(', 'static void IRAM_ATTR button_isr(')
advanced = cut(buttons, '    } else if (screen == objects.menu_advanced_screen) {',
               '    } else if (screen == objects.switch_dhcp_client_screen) {')

lvgl = r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef struct lv_event_t lv_event_t;
typedef struct lv_obj_t {
    struct lv_obj_t *parent, *child;
    bool hidden, focused, last;
    char text[64];
    void (*clicked)(lv_event_t *);
} lv_obj_t;
typedef unsigned lv_color_t;
typedef int lv_font_t;
typedef enum { LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_CLICKED } lv_event_code_t;
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_OBJ_FLAG_SNAPPABLE 2
#define LV_STATE_FOCUSED 4
#define LV_PART_MAIN 0
#define LV_ANIM_OFF 0
#define LV_LABEL_LONG_WRAP 0
#define LV_PCT(x) (x)
static const lv_font_t ui_font_font_book18;
static lv_obj_t advanced_screen, column, reboot_btn, reset_btn, usb_btn, usb_label, note;
static struct {
    lv_obj_t *menu_screen, *menu_advanced_screen, *menu_btn_advanced_reboot,
        *menu_btn_advanced_reset_config, *menu_btn_advanced_usb_emulation, *about_screen,
        *about_items_container, *status_items_container;
} objects = {NULL, &advanced_screen, &reboot_btn, &reset_btn, &usb_btn, NULL, NULL, NULL};
static char rpc_method[64], rpc_params[64];
static int rpcs, scrolls;
static void ui_call_rpc_handler(const char *method, const char *params)
{ strcpy(rpc_method, method); strcpy(rpc_params, params != NULL ? params : ""); rpcs++; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *obj, int index) { (void)index; return obj->child; }
static lv_obj_t *lv_obj_get_parent(lv_obj_t *obj) { return obj->parent; }
static void lv_obj_move_foreground(lv_obj_t *obj) { obj->last = true; }
static void lv_obj_add_flag(lv_obj_t *obj, int flag) { if (flag & LV_OBJ_FLAG_HIDDEN) obj->hidden = true; }
static void lv_obj_remove_flag(lv_obj_t *obj, int flag) { if (flag & LV_OBJ_FLAG_HIDDEN) obj->hidden = false; }
static bool lv_obj_has_state(lv_obj_t *obj, int state) { return state == LV_STATE_FOCUSED && obj->focused; }
static void lv_obj_add_event_cb(lv_obj_t *obj, void (*cb)(lv_event_t *), lv_event_code_t code, void *data)
{ (void)data; if (code == LV_EVENT_CLICKED) obj->clicked = cb; }
static void lv_obj_send_event(lv_obj_t *obj, lv_event_code_t code, void *param)
{ (void)param; if (code == LV_EVENT_CLICKED && obj->clicked != NULL) obj->clicked(NULL); }
static lv_obj_t *lv_label_create(lv_obj_t *parent) { note.parent = parent; return &note; }
static void lv_label_set_text(lv_obj_t *obj, const char *text) { strcpy(obj->text, text); }
static void lv_label_set_long_mode(lv_obj_t *obj, int mode) { (void)obj; (void)mode; }
static void lv_obj_set_width(lv_obj_t *obj, int width) { (void)obj; (void)width; }
static void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector)
{ (void)obj; (void)font; (void)selector; }
static lv_color_t lv_color_hex(unsigned hex) { return hex; }
static lv_color_t lv_color_white(void) { return 0xffffff; }
static void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
static void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
static void add_style_label_font16(lv_obj_t *obj) { (void)obj; }
static void lv_obj_update_layout(lv_obj_t *obj) { (void)obj; }
static void lv_obj_scroll_to_view_recursive(lv_obj_t *obj, int anim) { (void)anim; if (obj == &note) scrolls++; }
static int lv_obj_get_scroll_bottom(lv_obj_t *obj) { (void)obj; return 0; }
static void lv_obj_scroll_to_y(lv_obj_t *obj, int y, int anim) { (void)obj; (void)y; (void)anim; }
static void lv_obj_scroll_by(lv_obj_t *obj, int x, int y, int anim)
{ (void)obj; (void)x; (void)y; (void)anim; }
'''

prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "jetkvm_screen_usb_debugging.c"
#include "jetkvm_screen_button.h"
#define BUTTON_TOP 0U
#define BUTTON_BOTTOM 1U
#define BUTTON_HOLD_SHOW_MS 200U
#define TRACK_WIDTH 16
#define COLOR_ACTIVE 0x86b0ff
enum { SCREEN_ID_HOME_SCREEN = 1, SCREEN_ID_NO_NETWORK_SCREEN = 2 };
typedef struct { bool ready, active, dark; } local_display_state_t;
static lv_obj_t icon_top, icon_bottom, dot_top, dot_bottom, track_obj, fill_obj;
static lv_obj_t *top_icon = &icon_top, *bottom_icon = &icon_bottom, *top_dot = &dot_top,
    *bottom_dot = &dot_bottom, *track = &track_obj, *fill = &fill_obj;
static lv_obj_t *items[5];
static lv_obj_t *jetkvm_screen_regulatory_screen(void) { return NULL; }
static lv_obj_t *jetkvm_screen_regulatory_list(void) { return NULL; }
static lv_obj_t *hold_accept;
static unsigned selection, item_count;
static int pressed = -1;
static unsigned pressed_button;
static bool press_used;
static bool track_bottom;
static int fill_width;
static struct { jetkvm_screen_button_t state; } s_buttons[2];
static uint32_t held;
static bool local_display_state(local_display_state_t *state)
{ state->ready = state->active = true; state->dark = false; return true; }
static void local_display_wake(void) {}
uint32_t jetkvm_screen_button_held_ms(const jetkvm_screen_button_t *button) { (void)button; return held; }
static void place_track(void) { (void)track; (void)fill; }
static lv_obj_t *lv_screen_active(void) { return &advanced_screen; }
static bool wifi_setup_page(void) { return false; }
static bool wifi_setup_hold(void) { return false; }
static bool home(void) { return false; }
static void focus(void) {}
static void action_switch_to_menu(void *event) { (void)event; }
static void loadScreen(int id) { (void)id; }
static const char *get_var_main_screen(void) { return "home_screen"; }

/* The Advanced entries of changed(). */
static void list_items(lv_obj_t *screen)
{
    item_count = selection = 0;
    hold_accept = NULL;
    if (screen == NULL) {
'''

suffix = r'''
    }
}

static void hold(uint32_t hold_ms)
{
    buttons_press(BUTTON_BOTTOM, BUTTON_BOTTOM);
    for (held = 0; held <= hold_ms; held += 10) buttons_tick();
    held = 0;
    buttons_release();
}

static void expect(const char *label, bool note_shown)
{
    if (strcmp(usb_label.text, label) != 0 || note.hidden == note_shown) {
        fprintf(stderr, "entry \"%s\", note %s; expected \"%s\", note %s\n", usb_label.text,
                note.hidden ? "hidden" : "shown", label, note_shown ? "shown" : "hidden");
        assert(0);
    }
}

int main(void)
{
    usb_btn.hidden = true;
    usb_btn.parent = &column;
    usb_btn.child = &usb_label;
    strcpy(usb_label.text, "USB Emulation");

    jetkvm_screen_usb_debugging_init();
    assert(!usb_btn.hidden && usb_btn.last && note.parent == &column);
    assert(strcmp(note.text, "Takes effect at the next restart.") == 0);
    expect("USB Debugging: Off", false);

    /* The entry follows Reboot and Reset Config. */
    list_items(&advanced_screen);
    assert(item_count == 3 && items[0] == &reboot_btn && items[1] == &reset_btn &&
           items[2] == &usb_btn);

    /* Stored off, port off. A tap steps on; a hold shorter than Reboot's
     * does not act; the Reboot hold asks for the other value. */
    jetkvm_screen_usb_debugging_apply(false, false);
    expect("USB Debugging: Off", false);
    selection = 2;
    hold(50);
    assert(rpcs == 0 && selection == 0);
    selection = 2;
    hold(JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS - 100);
    assert(rpcs == 0);
    selection = 2;
    hold(JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 100);
    assert(rpcs == 1 && strcmp(rpc_method, "setUsbDebuggingState") == 0);
    assert(strcmp(rpc_params, "{\"enabled\":true}") == 0);

    /* Stored on, port on: the hold turns it off. */
    jetkvm_screen_usb_debugging_apply(true, true);
    expect("USB Debugging: On", false);
    selection = 2;
    hold(JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 100);
    assert(rpcs == 2 && strcmp(rpc_params, "{\"enabled\":false}") == 0);

    /* Changed from the web UI: the note shows until the restart, and comes
     * into view below the highlighted entry. */
    usb_btn.focused = true;
    jetkvm_screen_usb_debugging_apply(false, true);
    expect("USB Debugging: Off", true);
    assert(scrolls == 1);
    jetkvm_screen_usb_debugging_apply(true, false);
    expect("USB Debugging: On", true);
    jetkvm_screen_usb_debugging_apply(false, false);
    expect("USB Debugging: Off", false);

    puts("USB Debugging entry: after Reset Config, label and restart note follow the state, hold sends the other value: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'ui').mkdir()
    (path / 'lvgl.h').write_text(lvgl)
    for header in ('fonts.h', 'screens.h', 'styles.h', 'ui.h'):
        (path / 'ui' / header).write_text('#pragma once\n')
    # Next to the stubs, so its quoted includes find them first.
    for name in ('jetkvm_screen_usb_debugging.c', 'jetkvm_screen_usb_debugging.h'):
        (path / name).write_text((component / name).read_text())
    (path / 'test.c').write_text(prefix + advanced +
                                 suffix.replace('static void hold(', handlers + 'static void hold(', 1))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-Wno-unused-variable', '-fsanitize=address,undefined', f'-I{path}',
                    f'-I{component}', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
