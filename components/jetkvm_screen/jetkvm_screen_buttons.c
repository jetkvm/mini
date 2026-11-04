#include "jetkvm_screen_buttons.h"

#if CONFIG_JETKVM_SCREEN_BUTTONS

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "lvgl.h"

#include "jetkvm_board_config.h"
#include "jetkvm_screen_button.h"
#include "jetkvm_screen_orientation.h"
#include "jetkvm_screen_regulatory.h"
#include "local_display_settings.h"
#include "ui/actions.h"
#include "ui/fonts.h"
#include "ui/screens.h"
#include "ui/ui.h"
#include "ui/vars.h"

/*
 * Adapts the generated screens, their existing widgets and action callbacks
 * to the two push buttons beside the panel's native right edge, plus the
 * Regulatory page (jetkvm_screen_regulatory.c). No alternate screen tree is
 * created: menu entries receive the same LVGL events a touch would send.
 *
 * A strip beside the buttons shows one icon per button. When the picture is
 * turned, the strip stays beside the buttons on whichever canvas edge they
 * are on, its shapes keep their place on the panel, and its icons and text
 * are drawn upright. The button at the top or on the left as the reader sees
 * the screen is Back (Menu on the home screen); the other is Down or Scroll
 * on a tap, and a 700 ms hold accepts the highlighted entry or the Set up
 * Wi-Fi confirmation (jetkvm_screen_button_role()). Reboot Device and USB
 * Debugging (which stores the other setting and restarts) act on that hold
 * and Reset Config on a 10 s hold, without their generated confirmation
 * screens. A hold counts only while every sample reads the
 * button pressed and the pin does not change level. On the page of Wi-Fi
 * setup started from the menu, only a held Back button acts: it ends setup.
 *
 * Every press restarts the display's dim and off timers and brings the
 * backlight back to its set brightness. A press while the timers hold the
 * backlight at 0 only wakes the display: neither its hold nor its release
 * acts. A press on a dimmed display acts as usual.
 */

static const char *TAG = "screen_buttons";

/* Roles, as jetkvm_screen_button_role_t: BUTTON_TOP is Back and
 * BUTTON_BOTTOM is Down. s_buttons is indexed by button, SW3 first. */
#define BUTTON_TOP JETKVM_SCREEN_ROLE_BACK
#define BUTTON_BOTTOM JETKVM_SCREEN_ROLE_DOWN
#define BUTTON_COUNT 2U
/* A tap must not flash the hold bar; show it only once a press lasts this long. */
#define BUTTON_HOLD_SHOW_MS 200U

/* Strip parts are placed in the frame of jetkvm_screen_pillar_rect(): u runs
 * across the strip from its inner side, s along the button edge from SW3's
 * end. */
#define STRIP_WIDTH 26
/* Screen content keeps EDGE_PAD from the panel edges and STRIP_CLEAR from
 * the edge where the strip is. */
#define EDGE_PAD 12
#define STRIP_CLEAR (STRIP_WIDTH + 8)
#define TRACK_WIDTH 16
/* The hold bar sits beside the icon of the held button and fills toward
 * the button. */
#define TOP_TRACK_Y 100
#define BOTTOM_TRACK_Y 237
/* Pillar: a gray column that meets the buttons' edge and the edge at SW2's
 * end, with a rounded corner on its inner side at SW3's end. It overhangs the
 * panel at those two edges so only that one corner shows its radius. On the start screens a dot sits beside
 * each button and "Menu" runs between them; elsewhere the icons sit on it. */
#define PILLAR_TOP 52
#define PILLAR_RADIUS 12
/* The square mark at the left of the 116 px JetKVM logo. */
#define HOME_LOGO_MARK_WIDTH 32
#define TOP_ICON_Y 76
#define BOTTOM_ICON_Y 202
#define HOLD_HINT_Y 219
#define PILLAR_DOT 6
#define PILLAR_TOP_DOT_Y 81
#define PILLAR_BOTTOM_DOT_Y 207

#define COLOR_STRIP 0x111111
#define COLOR_DIM 0x94a3b8
#define COLOR_ACTIVE 0x86b0ff
#define COLOR_TRACK 0x29364b

#if LV_FONT_MONTSERRAT_16
#define ICON_FONT lv_font_montserrat_16
#else
#define ICON_FONT lv_font_montserrat_14
#endif

typedef struct {
    gpio_num_t pin;
    jetkvm_screen_button_t state;
} button_state_t;

/* SW3 sits above SW2 beside the panel. */
static button_state_t s_buttons[BUTTON_COUNT] = {
    {.pin = (gpio_num_t)JETKVM_BUTTON_SW3_PIN},
    {.pin = (gpio_num_t)JETKVM_BUTTON_SW2_PIN},
};
static TaskHandle_t s_task;
static uint32_t s_notify_bits;
/* One bit per button: its pin changed level since the last poll. */
static uint32_t s_changed;

static lv_obj_t *strip, *top_icon, *bottom_icon, *hold_hint, *track, *fill;
static lv_obj_t *home_marks, *top_dot, *bottom_dot, *menu_label;
static lv_obj_t *screens[13];
/* The clockwise turn of the picture on the panel. */
static uint16_t s_degrees;
/* The hold bar serves SW2 (else SW3) and shows this many pixels. */
static bool track_bottom;
static int fill_width;
/* Strip parts, placed again when the picture turns. */
typedef struct {
    lv_obj_t *obj;
    int16_t u, s, w, h;
    bool upright;
    /* A part of a role moves to the other button when the roles swap. */
    bool role;
} strip_part_t;
static strip_part_t parts[8];
static unsigned part_count;
static lv_obj_t *items[5];
/* A confirmation button accepted with the menu hold (CLICKED) instead of
 * its own countdown. */
static lv_obj_t *hold_accept;
static unsigned selection, item_count;
/* The role of the held button, and the button (index into s_buttons). */
static int pressed = -1;
static unsigned pressed_button;
static bool press_used;
static bool s_wifi_setup, s_wifi_setup_stoppable;

/* The no-network screen while Wi-Fi setup from the menu runs. */
static bool wifi_setup_page(void)
{
    return s_wifi_setup && lv_screen_active() == objects.no_network_screen;
}

static bool wifi_setup_hold(void)
{
    return pressed == BUTTON_TOP && s_wifi_setup_stoppable && wifi_setup_page();
}

static bool home(void)
{
    lv_obj_t *screen = lv_screen_active();
    return !wifi_setup_page() &&
           (screen == objects.home_screen || screen == objects.no_network_screen);
}

static lv_obj_t *plain(lv_obj_t *parent, int x, int y, int w, int h, unsigned color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

/* Moves `obj` to where the strip rectangle (u, s, w, h) lands on the canvas:
 * turned with the panel, or `upright` (w x h, centred on that place). */
static void place(lv_obj_t *obj, int u, int s, int w, int h, bool upright)
{
    jetkvm_screen_rect_t r = upright ? jetkvm_screen_pillar_upright(s_degrees, u, s, w, h)
                                     : jetkvm_screen_pillar_rect(s_degrees, u, s, w, h);
    lv_obj_set_pos(obj, r.x, r.y);
    lv_obj_set_size(obj, r.w, r.h);
}

static lv_obj_t *part(lv_obj_t *obj, int u, int s, int w, int h, bool upright, bool role)
{
    parts[part_count++] = (strip_part_t){obj, u, s, w, h, upright, role};
    place(obj, u, role ? jetkvm_screen_pillar_role_row(s_degrees, s) : s, w, h, upright);
    return obj;
}

static lv_obj_t *icon(int y, const lv_font_t *font, unsigned color)
{
    lv_obj_t *obj = lv_label_create(strip);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(obj, "");
    return part(obj, 0, y, STRIP_WIDTH, lv_font_get_line_height(font), true, true);
}

static lv_obj_t *dot(int y)
{
    lv_obj_t *obj = plain(home_marks, 0, 0, PILLAR_DOT, PILLAR_DOT, 0xffffff);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    return part(obj, (STRIP_WIDTH - PILLAR_DOT) / 2, y, PILLAR_DOT, PILLAR_DOT, false, true);
}

static void pillar_init(void)
{
    lv_obj_t *body = plain(strip, 0, 0, 1, 1, COLOR_STRIP);
    lv_obj_set_style_radius(body, PILLAR_RADIUS, 0);
    part(body, 0, PILLAR_TOP, STRIP_WIDTH + PILLAR_RADIUS, 240 - PILLAR_TOP + PILLAR_RADIUS, false, false);
    home_marks = plain(strip, 0, 0, 240, 240, COLOR_STRIP);
    lv_obj_set_style_bg_opa(home_marks, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(home_marks, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    top_dot = dot(PILLAR_TOP_DOT_Y);
    bottom_dot = dot(PILLAR_BOTTOM_DOT_Y);
    menu_label = lv_label_create(home_marks);
    lv_obj_set_style_text_font(menu_label, &ui_font_font_book16, 0);
    lv_obj_set_style_text_color(menu_label, lv_color_white(), 0);
    lv_label_set_text(menu_label, "Menu");
    lv_obj_add_flag(menu_label, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_update_layout(menu_label);
    int32_t w = lv_obj_get_width(menu_label);
    int32_t h = lv_obj_get_height(menu_label);
    int32_t center_y = (PILLAR_TOP_DOT_Y + PILLAR_BOTTOM_DOT_Y + PILLAR_DOT) / 2;
    part(menu_label, (STRIP_WIDTH - w) / 2, center_y - h / 2, w, h, true, false);
    lv_obj_set_style_transform_pivot_x(menu_label, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(menu_label, h / 2, 0);
}

/* Places the hold bar beside the held button's icon with `fill_width`
 * filled from its inner end toward the button. */
static void place_track(void)
{
    int u = (STRIP_WIDTH - TRACK_WIDTH) / 2;
    int s = jetkvm_screen_pillar_role_row(s_degrees, track_bottom ? BOTTOM_TRACK_Y : TOP_TRACK_Y);
    jetkvm_screen_rect_t bar = jetkvm_screen_pillar_rect(s_degrees, u, s, TRACK_WIDTH, 3);
    jetkvm_screen_rect_t done = jetkvm_screen_pillar_rect(s_degrees, u, s, fill_width, 3);
    lv_obj_set_pos(track, bar.x, bar.y);
    lv_obj_set_size(track, bar.w, bar.h);
    lv_obj_set_pos(fill, done.x - bar.x, done.y - bar.y);
    lv_obj_set_size(fill, done.w, done.h);
}

static void layout(void)
{
    for (unsigned i = 0; i < part_count; i++) {
        int s = parts[i].role ? jetkvm_screen_pillar_role_row(s_degrees, parts[i].s) : parts[i].s;
        place(parts[i].obj, parts[i].u, s, parts[i].w, parts[i].h, parts[i].upright);
    }
    place_track();
    jetkvm_screen_edge_t edge = jetkvm_screen_pillar_edge(s_degrees);
    bool vertical = edge == JETKVM_SCREEN_EDGE_RIGHT || edge == JETKVM_SCREEN_EDGE_LEFT;
    /* Beside a vertical strip "Menu" turns a quarter around its centre and
     * reads downward; along a horizontal strip it reads across. */
    lv_obj_set_style_transform_rotation(menu_label, vertical ? 900 : 0, 0);
    for (unsigned i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        lv_obj_set_style_pad_left(screens[i], edge == JETKVM_SCREEN_EDGE_LEFT ? STRIP_CLEAR : EDGE_PAD, 0);
        lv_obj_set_style_pad_right(screens[i], edge == JETKVM_SCREEN_EDGE_RIGHT ? STRIP_CLEAR : EDGE_PAD, 0);
        lv_obj_set_style_pad_top(screens[i], edge == JETKVM_SCREEN_EDGE_TOP ? STRIP_CLEAR : EDGE_PAD, 0);
        lv_obj_set_style_pad_bottom(screens[i], edge == JETKVM_SCREEN_EDGE_BOTTOM ? STRIP_CLEAR : EDGE_PAD, 0);
    }
    /* With the strip on the right, the home header ends above PILLAR_TOP, so
     * the header and its cloud status run to the right EDGE_PAD. The rows
     * below it keep clear of the pillar. */
    lv_obj_t *home_rows[] = {objects.home_info_container, objects.divider,
                             objects.home_status_container};
    for (unsigned i = 0; i < sizeof(home_rows) / sizeof(home_rows[0]); i++) {
        lv_obj_set_style_margin_right(home_rows[i],
                                      edge == JETKVM_SCREEN_EDGE_RIGHT ? STRIP_CLEAR - EDGE_PAD : 0, 0);
    }
    if (edge == JETKVM_SCREEN_EDGE_RIGHT) {
        lv_obj_set_style_pad_right(objects.home_screen, EDGE_PAD, 0);
    }
    /* With the strip on the left, the home header has 194 px: the logo, the
     * cloud icon and "N active" need 209, so the logo shows its mark without
     * the word. The generated logo is drawn up to 174 px past its own box,
     * so the cropped state draws it inside the box. */
    lv_obj_t *logo = objects.home_header_logo;
    lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_LEFT_MID);
    lv_obj_set_style_transform_width(logo, 0, LV_STATE_USER_1);
    lv_obj_set_style_transform_height(logo, 0, LV_STATE_USER_1);
    if (edge == JETKVM_SCREEN_EDGE_LEFT) {
        lv_obj_set_width(logo, HOME_LOGO_MARK_WIDTH);
        lv_obj_add_state(logo, LV_STATE_USER_1);
    } else {
        lv_obj_set_width(logo, LV_SIZE_CONTENT);
        lv_obj_remove_state(logo, LV_STATE_USER_1);
    }
}

static void focus(void)
{
    for (unsigned i = 0; i < item_count; i++) {
        if (i == selection) {
            lv_obj_add_state(items[i], LV_STATE_FOCUSED);
        } else {
            lv_obj_remove_state(items[i], LV_STATE_FOCUSED);
        }
    }
    if (item_count) {
        /* The entries sit in a column inside the scrolling container. */
        lv_obj_scroll_to_view_recursive(items[selection], LV_ANIM_OFF);
        /* A note below the last entry (USB Debugging's restart note) comes
         * into view with it. */
        lv_obj_t *note = lv_obj_get_sibling(items[selection], 1);
        if (selection + 1U == item_count && note != NULL &&
            !lv_obj_has_flag(note, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_scroll_to_view_recursive(note, LV_ANIM_OFF);
        }
    }
}

static void changed(lv_event_t *event)
{
    (void)event;
    lv_obj_t *screen = lv_screen_active();
    item_count = selection = 0;
    hold_accept = NULL;
    if (screen == objects.menu_screen) {
        /* In the order of jetkvm_screen_regulatory_init(). */
#if JETKVM_BOARD_WIFI
        items[item_count++] = objects.menu_btn_network; /* Set up Wi-Fi */
#endif
        items[item_count++] = jetkvm_screen_regulatory_entry();
        items[item_count++] = objects.menu_btn_advanced;
        items[item_count++] = objects.menu_btn_status;
        items[item_count++] = objects.menu_btn_about;
    } else if (screen == objects.menu_advanced_screen) {
        items[item_count++] = objects.menu_btn_advanced_reboot;
        items[item_count++] = objects.menu_btn_advanced_reset_config;
        items[item_count++] = objects.menu_btn_advanced_usb_emulation; /* USB Debugging */
    } else if (screen == objects.switch_dhcp_client_screen) {
        /* The Set up Wi-Fi confirmation on the Wi-Fi board. */
        hold_accept = objects.obj2;
    }
    /* Top button: open the menu on the home screen, end Wi-Fi setup on its
     * page, otherwise go back. */
    lv_label_set_text(top_icon, home()              ? LV_SYMBOL_LIST
                                : wifi_setup_page() ? (s_wifi_setup_stoppable ? LV_SYMBOL_CLOSE : "")
                                                    : LV_SYMBOL_LEFT);
    /* Bottom button: open the menu, step down, hold to accept, or scroll. */
    lv_label_set_text(bottom_icon, home()              ? LV_SYMBOL_LIST
                                   : wifi_setup_page() ? ""
                                   : hold_accept       ? LV_SYMBOL_OK
                                                       : LV_SYMBOL_DOWN);
    /* In a menu a held bottom button accepts the entry; the small check says so. */
    lv_label_set_text(hold_hint, item_count ? LV_SYMBOL_OK : "");
    lv_obj_set_style_text_color(top_icon, lv_color_white(), 0);
    lv_obj_set_style_text_color(bottom_icon, lv_color_white(), 0);
    lv_obj_add_flag(track, LV_OBJ_FLAG_HIDDEN);
    /* The start screens carry the dots and "Menu" on the pillar; other
     * screens carry the icons. */
    if (home()) {
        lv_obj_remove_flag(home_marks, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bottom_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(home_marks, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bottom_icon, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(top_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_color(bottom_dot, lv_color_white(), 0);
    if (screen == objects.boot_screen || screen == objects.rebooting_screen) {
        lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(strip, LV_OBJ_FLAG_HIDDEN);
    }
    focus();
}

static void no_grow(lv_obj_t *button)
{
    /* Synthetic PRESSED events put the row in the pressed state, where the
     * theme's grow transform pushes it past its container and the clipped
     * corners read as a square box. Keep the pressed size unchanged. */
    lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
}

static void menu_style(lv_obj_t **buttons, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        no_grow(buttons[i]);
        /* Fill only: an outline sits outside the row and the container clips it. */
        lv_obj_set_style_bg_color(buttons[i], lv_color_hex(0x1d4ed8), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(buttons[i], 0, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(buttons[i], 0, LV_STATE_FOCUSED);
    }
}

static void hide_back_button(lv_obj_t *header)
{
    /* The generated header is a flex row of a back button and the title;
     * without the touch target the title takes the row's start. */
    lv_obj_t *button = lv_obj_get_child(header, 0);
    if (button != NULL) {
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static void buttons_fit(void)
{
    /* Reserve the panel edge beside the buttons for the button strip (see
     * layout()) while keeping the generated components, fonts, header and
     * styling. */
    lv_obj_t *all[] = {objects.boot_screen, objects.no_network_screen, objects.home_screen,
        objects.menu_screen, objects.menu_advanced_screen, objects.menu_network_screen,
        objects.about_screen, objects.status_screen, objects.reset_config_screen,
        objects.reboot_screen, objects.rebooting_screen, objects.switch_dhcp_client_screen,
        jetkvm_screen_regulatory_screen()};
    _Static_assert(sizeof(all) == sizeof(screens), "screens holds every screen");
    for (unsigned i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        screens[i] = all[i];
        lv_obj_set_size(screens[i], 240, 240);
        lv_obj_add_event_cb(screens[i], changed, LV_EVENT_SCREEN_LOADED, NULL);
    }
    lv_obj_t *main_items[] = {objects.menu_btn_status, jetkvm_screen_regulatory_entry(),
                              objects.menu_btn_network, objects.menu_btn_advanced,
                              objects.menu_btn_about};
    lv_obj_t *advanced_items[] = {objects.menu_btn_advanced_reboot,
                                  objects.menu_btn_advanced_reset_config,
                                  objects.menu_btn_advanced_usb_emulation};
    menu_style(main_items, 5);
    menu_style(advanced_items, 3);
    /* The Mini has one DHCP client, so the RV1106's client switch is hidden. */
    lv_obj_add_flag(objects.menu_btn_dhcp_client, LV_OBJ_FLAG_HIDDEN);
    /* The generated "Reset Configuration" does not fit beside a vertical
     * strip in the 20 px face. */
    lv_obj_t *reset_label = lv_obj_get_child(objects.menu_btn_advanced_reset_config, 0);
    if (reset_label != NULL && lv_obj_check_type(reset_label, &lv_label_class)) {
        lv_label_set_text(reset_label, "Reset Config");
    }
    lv_obj_t *headers[] = {objects.menu_header_container, objects.menu_header_container_1,
        objects.menu_header_container_2, objects.about_header_container,
        objects.status_header_container, objects.reset_config_header, objects.reboot_header,
        objects.dhcp_client_header};
    for (unsigned i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
        hide_back_button(headers[i]);
    }

    /* The strip spans the canvas; its parts sit beside the buttons. */
    strip = plain(lv_layer_top(), 0, 0, 240, 240, COLOR_STRIP);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    pillar_init();
    /* One icon per role, laid out as at 0 degrees: Back beside SW3, Down
     * beside SW2. */
    top_icon = icon(TOP_ICON_Y, &ICON_FONT, 0xffffff);
    bottom_icon = icon(BOTTOM_ICON_Y, &ICON_FONT, 0xffffff);
    hold_hint = icon(HOLD_HINT_Y, &lv_font_montserrat_14, COLOR_DIM);
    track = plain(strip, 0, 0, 1, 1, COLOR_TRACK);
    fill = plain(track, 0, 0, 1, 1, COLOR_ACTIVE);
    track_bottom = true;
    fill_width = 2;
    layout();
    changed(NULL);
}

static void buttons_press(unsigned side, unsigned button)
{
    if (pressed >= 0 || side > 1) {
        return;
    }
    pressed = (int)side;
    pressed_button = button;
    local_display_state_t display;
    /* A press that lights a dark display counts as one that has already acted. */
    press_used = local_display_state(&display) && display.dark;
    local_display_wake();
    lv_obj_set_style_text_color(side == BUTTON_TOP ? top_icon : bottom_icon,
                                lv_color_hex(COLOR_ACTIVE), 0);
    lv_obj_set_style_bg_color(side == BUTTON_TOP ? top_dot : bottom_dot,
                              lv_color_hex(COLOR_ACTIVE), 0);
    track_bottom = side == BUTTON_BOTTOM;
    fill_width = 2;
    place_track();
}

static void back(void)
{
    if (lv_screen_active() == objects.menu_screen) {
        loadScreen(strcmp(get_var_main_screen(), "home_screen") == 0
                       ? SCREEN_ID_HOME_SCREEN
                       : SCREEN_ID_NO_NETWORK_SCREEN);
    } else {
        action_switch_to_menu(NULL);
    }
}

/* Reset Config erases the settings, so it needs a hold that a glitch on the
 * pin cannot produce. */
static uint32_t hold_ms(void)
{
    return item_count && items[selection] == objects.menu_btn_advanced_reset_config
               ? JETKVM_SCREEN_BUTTON_RESET_HOLD_MS
               : JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS;
}

static void buttons_tick(void)
{
    if (pressed < 0 || press_used) {
        return;
    }
    uint32_t elapsed = jetkvm_screen_button_held_ms(&s_buttons[pressed_button].state);
    uint32_t needed = hold_ms();
    if ((pressed == BUTTON_BOTTOM && (item_count || hold_accept)) || wifi_setup_hold()) {
        if (elapsed >= BUTTON_HOLD_SHOW_MS) {
            fill_width = elapsed < needed ? (int)(TRACK_WIDTH * elapsed / needed) : TRACK_WIDTH;
            place_track();
            lv_obj_remove_flag(track, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* A released sample or a sampling gap restarted the hold. */
            lv_obj_add_flag(track, LV_OBJ_FLAG_HIDDEN);
        }
        if (elapsed >= needed) {
            press_used = true;
            if (wifi_setup_hold()) {
                /* The icon stays until the state reports that setup ended. */
                ui_call_rpc_handler("stopWifiSetup", NULL);
                return;
            }
            if (hold_accept != NULL) {
                lv_obj_send_event(hold_accept, LV_EVENT_CLICKED, NULL);
                return;
            }
            lv_obj_t *target = items[selection];
            /* The hold is the confirmation: run the maintenance action the
             * generated confirmation screen would run after its countdown. */
            if (target == objects.menu_btn_advanced_reboot) {
                ui_call_rpc_handler("reboot", NULL);
                return;
            }
            if (target == objects.menu_btn_advanced_reset_config) {
                ui_call_rpc_handler("resetConfig", NULL);
                return;
            }
            /* The generated menu callbacks listen for PRESSED or CLICKED. */
            lv_obj_send_event(target, LV_EVENT_PRESSED, NULL);
            lv_obj_send_event(target, LV_EVENT_RELEASED, NULL);
            lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
        }
    }
}

static void buttons_release(void)
{
    if (pressed < 0) {
        return;
    }
    int side = pressed;
    pressed = -1;
    lv_obj_set_style_text_color(top_icon, lv_color_white(), 0);
    lv_obj_set_style_text_color(bottom_icon, lv_color_white(), 0);
    lv_obj_set_style_bg_color(top_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_color(bottom_dot, lv_color_white(), 0);
    lv_obj_add_flag(track, LV_OBJ_FLAG_HIDDEN);
    if (press_used) {
        /* The hold has acted or the press lit a dark display; the release
         * is not a tap. */
        return;
    }
    if (wifi_setup_page() || (side == BUTTON_BOTTOM && hold_accept != NULL)) {
        /* A tap must not end setup while the user types on the phone, nor
         * accept a confirmation. */
        return;
    }
    if (home()) {
        action_switch_to_menu(NULL);
    } else if (side == BUTTON_TOP) {
        back();
    } else if (item_count) {
        selection = (selection + 1) % item_count;
        focus();
    } else {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_t *list = screen == objects.about_screen ? objects.about_items_container
                         : screen == jetkvm_screen_regulatory_screen()
                             ? jetkvm_screen_regulatory_list()
                             : objects.status_items_container;
        if (lv_obj_get_scroll_bottom(list) <= 0) {
            lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
        } else {
            lv_obj_scroll_by(list, 0, -60, LV_ANIM_OFF);
        }
    }
}

/* The HDMI preview covers the UI, so a press there only wakes the display.
 * A press that started on the UI counts as used, so neither its hold nor its
 * release acts on the UI, also when the preview ends while it is held. */
static void buttons_preview_edge(unsigned button, jetkvm_screen_button_edge_t edge)
{
    if (pressed >= 0) {
        press_used = true;
    }
    if (edge == JETKVM_SCREEN_BUTTON_PRESSED) {
        local_display_wake();
    } else if (edge == JETKVM_SCREEN_BUTTON_RELEASED && pressed >= 0 && pressed_button == button) {
        buttons_release();
    }
}

static void IRAM_ATTR button_isr(void *arg)
{
    __atomic_fetch_or(&s_changed, 1U << (uintptr_t)arg, __ATOMIC_RELAXED);
    BaseType_t woken = pdFALSE;
    if (s_task != NULL) {
        xTaskNotifyFromISR(s_task, s_notify_bits, eSetBits, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

void jetkvm_screen_buttons_init(TaskHandle_t task, uint32_t notify_bits)
{
    if (strip != NULL) {
        return;
    }
    s_task = task;
    s_notify_bits = notify_bits;
    buttons_fit();

    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << JETKVM_BUTTON_SW2_PIN) | (1ULL << JETKVM_BUTTON_SW3_PIN),
        .mode = GPIO_MODE_INPUT,
        /* Both buttons carry an external 10 kOhm pull-up to ESP_3V3. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    esp_err_t error = gpio_install_isr_service(0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(error);
    }
    for (unsigned i = 0; i < BUTTON_COUNT; i++) {
        jetkvm_screen_button_init(&s_buttons[i].state, gpio_get_level(s_buttons[i].pin) == 0,
                                  lv_tick_get());
        ESP_ERROR_CHECK(gpio_isr_handler_add(s_buttons[i].pin, button_isr, (void *)(uintptr_t)i));
    }
    ESP_LOGI(TAG, "SW3=GPIO%d top, SW2=GPIO%d bottom drive the local UI",
             JETKVM_BUTTON_SW3_PIN, JETKVM_BUTTON_SW2_PIN);
}

void jetkvm_screen_buttons_set_rotation(uint16_t degrees)
{
    if (degrees == s_degrees) {
        return;
    }
    s_degrees = degrees;
    if (strip != NULL) {
        layout();
    }
}

void jetkvm_screen_buttons_set_wifi_setup(bool active, bool stoppable)
{
    if (strip == NULL || (active == s_wifi_setup && stoppable == s_wifi_setup_stoppable)) {
        return;
    }
    s_wifi_setup = active;
    s_wifi_setup_stoppable = stoppable;
    changed(NULL);
}

bool jetkvm_screen_buttons_poll(bool preview)
{
    bool busy = false;
    uint32_t changed = __atomic_exchange_n(&s_changed, 0, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < BUTTON_COUNT; i++) {
        jetkvm_screen_button_t *button = &s_buttons[i].state;
        jetkvm_screen_button_edge_t edge = jetkvm_screen_button_sample(
            button, gpio_get_level(s_buttons[i].pin) == 0, (changed >> i) & 1U, lv_tick_get());
        if (preview) {
            buttons_preview_edge(i, edge);
        } else if (edge == JETKVM_SCREEN_BUTTON_PRESSED) {
            /* The role is fixed for the whole press, even if the picture turns. */
            buttons_press(jetkvm_screen_button_role(
                              s_degrees, i == 0 ? JETKVM_SCREEN_BUTTON_SW3 : JETKVM_SCREEN_BUTTON_SW2),
                          i);
        } else if (edge == JETKVM_SCREEN_BUTTON_RELEASED && pressed >= 0 && pressed_button == i) {
            buttons_release();
        }
        if (button->stable_pressed != button->raw_pressed || button->stable_pressed) {
            busy = true;
        }
    }
    if (!preview) {
        buttons_tick();
    }
    return busy;
}

#endif /* CONFIG_JETKVM_SCREEN_BUTTONS */
