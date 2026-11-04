#!/usr/bin/env python3
"""Run the production button press, hold and release handlers against a
display that the off timer has made dark, one that the dim timer has dimmed,
a lit one, and the HDMI preview."""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[2]
source = (component / 'jetkvm_screen_buttons.c').read_text()
start = source.index('static void buttons_press(')
end = source.index('static void IRAM_ATTR button_isr(', start)

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "jetkvm_screen_button.h"
typedef struct { int id; } lv_obj_t;
typedef unsigned lv_color_t;
typedef enum { LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_CLICKED } lv_event_code_t;
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_ANIM_OFF 0
#define BUTTON_TOP 0U
#define BUTTON_BOTTOM 1U
#define BUTTON_HOLD_SHOW_MS 200U
#define TRACK_WIDTH 16
#define TOP_TRACK_Y 100
#define BOTTOM_TRACK_Y 237
#define COLOR_ACTIVE 0x86b0ff
enum { SCREEN_ID_HOME_SCREEN = 1, SCREEN_ID_NO_NETWORK_SCREEN = 2 };
typedef struct { bool ready, active, dark; } local_display_state_t;

static lv_obj_t home_screen, menu_screen, advanced_screen, about_screen, regulatory_screen;
static lv_obj_t status_btn, about_btn, reboot_btn, reset_btn, list, regulatory_list;
static struct {
    lv_obj_t *menu_screen, *menu_btn_advanced_reboot, *menu_btn_advanced_reset_config,
        *about_screen, *about_items_container, *status_items_container;
} objects = {&menu_screen, &reboot_btn, &reset_btn, &about_screen, &list, &list};
static lv_obj_t icon_top, icon_bottom, dot_top, dot_bottom, track_obj, fill_obj;
static lv_obj_t *top_icon = &icon_top, *bottom_icon = &icon_bottom, *top_dot = &dot_top,
    *bottom_dot = &dot_bottom, *track = &track_obj, *fill = &fill_obj;
static lv_obj_t *items[5];
static lv_obj_t *hold_accept;
static unsigned selection, item_count;
static int pressed = -1;
static unsigned pressed_button;
static bool press_used;
static bool track_bottom;
static int fill_width;
static struct { jetkvm_screen_button_t state; } s_buttons[2];

static lv_obj_t *active_screen;
/* The backlight level that the timers have left: lit, dimmed or dark. */
static enum { LIT, DIMMED, DARK } backlight;
static int wakes, menu_opens, back_loads, events, rpcs;
static uint32_t held;

static bool local_display_state(local_display_state_t *state)
{ state->ready = state->active = true; state->dark = backlight == DARK; return true; }
static void local_display_wake(void) { wakes++; backlight = LIT; }
uint32_t jetkvm_screen_button_held_ms(const jetkvm_screen_button_t *button) { (void)button; return held; }
static void place_track(void) { (void)track; (void)fill; }
static lv_obj_t *lv_screen_active(void) { return active_screen; }
static bool wifi_setup_page(void) { return false; }
static bool wifi_setup_hold(void) { return false; }
static bool home(void) { return active_screen == &home_screen; }
static void focus(void) {}
static void action_switch_to_menu(void *event) { (void)event; menu_opens++; }
static void loadScreen(int id) { (void)id; back_loads++; }
static const char *get_var_main_screen(void) { return "home_screen"; }
static void ui_call_rpc_handler(const char *method, void *params) { (void)method; (void)params; rpcs++; }
static void lv_obj_send_event(lv_obj_t *obj, lv_event_code_t code, void *param)
{ (void)obj; (void)code; (void)param; events++; }
static lv_color_t lv_color_hex(unsigned hex) { return hex; }
static lv_color_t lv_color_white(void) { return 0xffffff; }
static void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
static void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
static lv_obj_t *jetkvm_screen_regulatory_screen(void) { return &regulatory_screen; }
static lv_obj_t *jetkvm_screen_regulatory_list(void) { return &regulatory_list; }
static lv_obj_t *scrolled;
static void lv_obj_set_y(lv_obj_t *obj, int y) { (void)obj; (void)y; }
static void lv_obj_set_width(lv_obj_t *obj, int width) { (void)obj; (void)width; }
static void lv_obj_add_flag(lv_obj_t *obj, int flag) { (void)obj; (void)flag; }
static void lv_obj_remove_flag(lv_obj_t *obj, int flag) { (void)obj; (void)flag; }
static int lv_obj_get_scroll_bottom(lv_obj_t *obj) { (void)obj; return 60; }
static void lv_obj_scroll_to_y(lv_obj_t *obj, int y, int anim) { (void)obj; (void)y; (void)anim; }
static void lv_obj_scroll_by(lv_obj_t *obj, int x, int y, int anim)
{ (void)x; (void)y; (void)anim; scrolled = obj; }
'''

suffix = r'''
/* One press of `side` held for `hold_ms`, with the screen loop ticking every
 * 10 ms, then released. At 0 degrees the button with index `side` has that
 * role. */
static void press(unsigned side, uint32_t hold_ms)
{
    buttons_press(side, side);
    for (held = 0; held <= hold_ms; held += 10) buttons_tick();
    held = 0;
    buttons_release();
}

static void show(lv_obj_t *screen, lv_obj_t *first, lv_obj_t *second)
{
    active_screen = screen;
    items[0] = first;
    items[1] = second;
    item_count = 2;
    selection = 0;
    hold_accept = NULL;
}

static void reset_counts(void) { wakes = menu_opens = back_loads = events = rpcs = 0; }

int main(void)
{
    /* Home screen: a press on a dark display only wakes it; a press on a
     * dimmed display wakes it and opens the menu. */
    for (unsigned side = BUTTON_TOP; side <= BUTTON_BOTTOM; side++) {
        active_screen = &home_screen;
        item_count = 0;
        reset_counts();
        backlight = DARK;
        press(side, 50);
        assert(wakes == 1 && backlight == LIT && menu_opens == 0);
        press(side, 50);
        assert(wakes == 2 && menu_opens == 1);
        backlight = DIMMED;
        press(side, 50);
        assert(wakes == 3 && backlight == LIT && menu_opens == 2);
    }

    /* Menu: a top tap on a dark display does not go back; on a dimmed
     * display it does. */
    show(&menu_screen, &status_btn, &about_btn);
    reset_counts();
    backlight = DARK;
    press(BUTTON_TOP, 50);
    assert(wakes == 1 && back_loads == 0 && menu_opens == 0);
    backlight = DIMMED;
    press(BUTTON_TOP, 50);
    assert(wakes == 2 && backlight == LIT && back_loads == 1);

    /* Menu: on a dark display a bottom tap does not step down and a hold past
     * the accept time does not accept the entry; on a dimmed display both act. */
    show(&menu_screen, &status_btn, &about_btn);
    reset_counts();
    backlight = DARK;
    press(BUTTON_BOTTOM, 50);
    assert(wakes == 1 && selection == 0);
    backlight = DARK;
    press(BUTTON_BOTTOM, JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 100);
    assert(wakes == 2 && events == 0 && selection == 0);
    backlight = DIMMED;
    press(BUTTON_BOTTOM, 50);
    assert(wakes == 3 && selection == 1);
    backlight = DIMMED;
    press(BUTTON_BOTTOM, JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 100);
    assert(wakes == 4 && events == 3);

    /* Advanced menu: a hold on a dark display does not reboot or reset the
     * settings; a hold on a dimmed display resets them. */
    show(&advanced_screen, &reboot_btn, &reset_btn);
    reset_counts();
    backlight = DARK;
    press(BUTTON_BOTTOM, JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS + 100);
    assert(rpcs == 0);
    selection = 1;
    backlight = DARK;
    press(BUTTON_BOTTOM, JETKVM_SCREEN_BUTTON_RESET_HOLD_MS + 100);
    assert(rpcs == 0 && wakes == 2);
    backlight = DIMMED;
    press(BUTTON_BOTTOM, JETKVM_SCREEN_BUTTON_RESET_HOLD_MS + 100);
    assert(rpcs == 1 && wakes == 3);

    /* Regulatory: a bottom tap scrolls its text, not the Status list; a top
     * tap goes back to the menu. */
    active_screen = &regulatory_screen;
    item_count = 0;
    hold_accept = NULL;
    reset_counts();
    backlight = LIT;
    press(BUTTON_BOTTOM, 50);
    assert(scrolled == &regulatory_list);
    press(BUTTON_TOP, 50);
    assert(menu_opens == 1);

    /* HDMI preview: a press only wakes the display, and a press that started
     * on the UI before the preview does not act on its release. */
    active_screen = &home_screen;
    item_count = 0;
    reset_counts();
    backlight = DIMMED;
    buttons_preview_edge(BUTTON_TOP, JETKVM_SCREEN_BUTTON_PRESSED);
    buttons_preview_edge(BUTTON_TOP, JETKVM_SCREEN_BUTTON_RELEASED);
    assert(wakes == 1 && backlight == LIT && menu_opens == 0);
    buttons_press(BUTTON_TOP, BUTTON_TOP);
    buttons_preview_edge(BUTTON_TOP, JETKVM_SCREEN_BUTTON_RELEASED);
    assert(wakes == 2 && menu_opens == 0 && pressed < 0);

    /* A bottom press on Reset Config that the preview interrupts does not
     * reset the settings when the preview ends while it is still held. */
    show(&advanced_screen, &reboot_btn, &reset_btn);
    selection = 1;
    reset_counts();
    buttons_press(BUTTON_BOTTOM, BUTTON_BOTTOM);
    buttons_preview_edge(BUTTON_BOTTOM, JETKVM_SCREEN_BUTTON_NONE);
    held = JETKVM_SCREEN_BUTTON_RESET_HOLD_MS + 100;
    buttons_tick();
    held = 0;
    buttons_release();
    assert(rpcs == 0 && events == 0 && selection == 1);

    puts("Button press on a dark display or the HDMI preview only wakes it; on a dimmed display it wakes and acts: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + source[start:end] + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', f'-I{component}', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
