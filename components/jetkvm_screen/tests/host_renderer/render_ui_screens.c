#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <lvgl.h>

#include "host_ui.h"
#include "jetkvm_screen_buttons.h"
#include "jetkvm_screen_fit.h"
#include "jetkvm_screen_home_address.h"
#include "jetkvm_screen_regulatory.h"
#include "jetkvm_screen_usb_debugging.h"
#include "fonts.h"
#include "screens.h"
#include "ui.h"
#include "vars.h"

#define PANEL_WIDTH JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH
#define PANEL_HEIGHT JETKVM_SCREEN_PANEL_LANDSCAPE_HEIGHT
/* The bottom button at 0 degrees: Down, and Scroll on a page of text. */
#define BUTTON_DOWN 1U

static void add_representative_wifi_status(void)
{
    lv_obj_add_flag(objects.home_info_ipv6_addr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.home_info_mac_addr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *row = lv_obj_create(lv_obj_get_parent(objects.home_info_mac_addr));
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(98), 17);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);

    lv_obj_t *signal = lv_label_create(row);
    lv_obj_set_style_text_font(signal, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(signal, lv_color_hex(0x22c55e), LV_PART_MAIN);
    lv_label_set_text(signal, LV_SYMBOL_WIFI);

    lv_obj_t *ssid = lv_label_create(row);
    lv_obj_set_width(ssid, 0);
    lv_obj_set_height(ssid, 17);
    lv_obj_set_flex_grow(ssid, 1);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(ssid, &ui_font_font_book16, LV_PART_MAIN);
    lv_obj_set_style_text_color(ssid, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_label_set_text(ssid, "JetKVM-Lab-5G");
}

static void set_representative_text(void)
{
    jetkvm_screen_home_address_set("192.168.0.105");
    add_representative_wifi_status();
    lv_label_set_text(objects.usb_status_label, "Connected");
    lv_label_set_text(objects.hdmi_status_label, "Connected");
    lv_label_set_text(objects.system_version, "ESP-IDF 6.0.2");
    lv_label_set_text(objects.app_version, "kvm-mini local render");
    lv_label_set_text(objects.build_branch, "main");
    lv_label_set_text(objects.build_date, "2026-09-04");
    lv_label_set_text(objects.kernel_version, "ESP32-P4");
    lv_label_set_text(objects.cpu_serial, "80:F1:B2:D6:31:D9");
    lv_label_set_text(objects.device_id, "kvm-mini-p4");
    lv_label_set_text(objects.app_version_1, "Not configured");
    lv_label_set_text(objects.cloud_domain, "app.jetkvm.com");
    lv_label_set_text(objects.cloud_status_label, "1 active");
#if JETKVM_BOARD_WIFI
    /* The Network entry is Set up Wi-Fi on the Wi-Fi board. */
    lv_label_set_text(lv_obj_get_child(objects.menu_btn_network, 0), "Set up Wi-Fi");
#else
    lv_obj_add_flag(objects.menu_btn_network, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_obj_add_flag(objects.menu_btn_access, LV_OBJ_FLAG_HIDDEN);
}

static int write_ppm(const char *path)
{
    FILE *output = fopen(path, "wb");
    if (output == NULL) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(output, "P6\n%u %u\n255\n", (unsigned)PANEL_WIDTH,
            (unsigned)PANEL_HEIGHT);

    for (uint16_t y = 0; y < PANEL_HEIGHT; ++y) {
        for (uint16_t x = 0; x < PANEL_WIDTH; ++x) {
            const uint16_t source_x = jetkvm_screen_fit_source_x(x);
            const uint16_t pixel =
                host_ui_framebuffer[(size_t)y * JETKVM_SCREEN_SOURCE_WIDTH + source_x];
            const uint8_t rgb[3] = {
                (uint8_t)((((pixel >> 11U) & 0x1fU) * 255U) / 31U),
                (uint8_t)((((pixel >> 5U) & 0x3fU) * 255U) / 63U),
                (uint8_t)(((pixel & 0x1fU) * 255U) / 31U),
            };
            if (fwrite(rgb, sizeof(rgb), 1, output) != 1) {
                fprintf(stderr, "cannot write %s\n", path);
                fclose(output);
                return -1;
            }
        }
    }
    return fclose(output);
}

/* Writes the active screen; `id` only numbers the file. */
static int render_active(const char *output_dir, int id, const char *name)
{
    char path[1024];
    const int length = snprintf(path, sizeof(path), "%s/%02d-%s.ppm", output_dir,
                                id, name);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        return -1;
    }

    /* A state change, such as the highlight of a menu entry, starts a short
     * colour transition of the theme. Draw its end, as the reader sees it. */
    for (unsigned i = 0; i < 3; ++i) {
        lv_tick_inc(200);
        lv_timer_handler();
    }
    memset(host_ui_framebuffer, 0, sizeof(host_ui_framebuffer));
    lv_obj_invalidate(lv_screen_active());
    lv_obj_update_layout(lv_screen_active());
    if (lv_obj_get_width(lv_screen_active()) != (int32_t)PANEL_WIDTH ||
        lv_obj_get_height(lv_screen_active()) != (int32_t)PANEL_HEIGHT) {
        fprintf(stderr, "screen %d (%s) is not %ux%u\n", id, name,
                (unsigned)PANEL_WIDTH, (unsigned)PANEL_HEIGHT);
        return -1;
    }
    lv_refr_now(NULL);
    return write_ppm(path);
}

static int render_screen(const char *output_dir, int id, const char *name)
{
    loadScreen((enum ScreensEnum)id);
    return render_active(output_dir, id, name);
}

int main(int argc, char **argv)
{
    static const char *screen_names[] = {
        "boot",          "no-network", "home",      "menu",
        "advanced",      "network",    "about",     "status",
        "reset-config",  "reboot",     "rebooting", "dhcp-client",
    };

    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT_DIRECTORY\n", argv[0]);
        return 2;
    }
    if (mkdir(argv[1], 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    if (!host_ui_start()) {
        fprintf(stderr, "cannot create LVGL display\n");
        return 1;
    }
    set_representative_text();
    /* Both boards have the two buttons, so each page is drawn beside the
     * button strip, as on the device at 0 degrees. */
    host_ui_start_buttons();

    for (int id = SCREEN_ID_BOOT_SCREEN; id <= SCREEN_ID_SWITCH_DHCP_CLIENT_SCREEN;
         ++id) {
        if (render_screen(argv[1], id, screen_names[id - 1]) != 0) {
            return 1;
        }
    }
    /* Advanced > USB Debugging highlighted: the third entry, two taps down. */
    static const struct {
        const char *name;
        bool enabled, active;
    } usb_debugging[] = {
        {"advanced-usb-debugging-on", true, true},
        {"advanced-usb-debugging-off", false, false},
        {"advanced-usb-debugging-restart", true, false},
    };
    for (size_t i = 0; i < sizeof(usb_debugging) / sizeof(usb_debugging[0]); ++i) {
        jetkvm_screen_usb_debugging_apply(usb_debugging[i].enabled, usb_debugging[i].active);
        /* Opening the page puts the highlight on its first entry. */
        loadScreen(SCREEN_ID_MENU_SCREEN);
        loadScreen(SCREEN_ID_MENU_ADVANCED_SCREEN);
        host_ui_tap(BUTTON_DOWN);
        host_ui_tap(BUTTON_DOWN);
        if (!lv_obj_has_state(objects.menu_btn_advanced_usb_emulation, LV_STATE_FOCUSED) ||
            render_active(argv[1], SCREEN_ID_MENU_ADVANCED_SCREEN, usb_debugging[i].name) != 0) {
            return 1;
        }
    }

#if JETKVM_BOARD_WIFI
    /* Menu with Regulatory highlighted (the second entry, one tap down),
     * then the page at its top and after each bottom-button scroll. */
    lv_obj_t *entry = jetkvm_screen_regulatory_entry();
    lv_obj_t *regulatory = jetkvm_screen_regulatory_screen();
    loadScreen(SCREEN_ID_MENU_SCREEN);
    host_ui_tap(BUTTON_DOWN);
    if (!lv_obj_has_state(entry, LV_STATE_FOCUSED) ||
        render_active(argv[1], SCREEN_ID_MENU_SCREEN, "menu-regulatory") != 0) {
        return 1;
    }
    lv_obj_send_event(entry, LV_EVENT_CLICKED, NULL);
    if (lv_screen_active() != regulatory ||
        render_active(argv[1], 13, "regulatory") != 0) {
        return 1;
    }
    for (int page = 1; lv_obj_get_scroll_bottom(jetkvm_screen_regulatory_list()) > 0; ++page) {
        char name[32];
        snprintf(name, sizeof(name), "regulatory-scroll-%d", page);
        host_ui_tap(BUTTON_DOWN);
        if (render_active(argv[1], 13, name) != 0) {
            return 1;
        }
    }
#else
    /* The Ethernet board has no Regulatory entry and no such page: the menu
     * holds Advanced, Status and About, in that order, beside the hidden
     * generated entries. */
    if (jetkvm_screen_regulatory_entry() != NULL || jetkvm_screen_regulatory_screen() != NULL) {
        fprintf(stderr, "the Ethernet board has a Regulatory page\n");
        return 1;
    }
    static const char *const menu[] = {"Advanced", "Status", "About"};
    lv_obj_t *entries = lv_obj_get_parent(objects.menu_btn_status);
    size_t shown = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(entries); ++i) {
        lv_obj_t *entry = lv_obj_get_child(entries, i);
        if (lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        const char *text = lv_label_get_text(lv_obj_get_child(entry, 0));
        if (shown >= sizeof(menu) / sizeof(menu[0]) || strcmp(text, menu[shown]) != 0) {
            fprintf(stderr, "menu entry %zu is \"%s\"\n", shown + 1, text);
            return 1;
        }
        ++shown;
    }
    if (shown != sizeof(menu) / sizeof(menu[0])) {
        fprintf(stderr, "the menu shows %zu entries\n", shown);
        return 1;
    }
    if (render_screen(argv[1], SCREEN_ID_MENU_SCREEN, "menu-ethernet") != 0) {
        return 1;
    }
#endif

    /* The home screen with the address that is widest in the layout's font,
     * with the strip at each canvas edge. */
    char widest[16];
    host_ui_widest_address(&ui_font_font_bold30, widest);
    jetkvm_screen_home_address_set(widest);
    loadScreen(SCREEN_ID_HOME_SCREEN);
    static const uint16_t turns[] = {0, 90, 180, 270};
    for (size_t i = 0; i < sizeof(turns) / sizeof(turns[0]); ++i) {
        char name[32];
        snprintf(name, sizeof(name), "home-widest-address-%u", (unsigned)turns[i]);
        jetkvm_screen_buttons_set_rotation(turns[i]);
        if (render_active(argv[1], SCREEN_ID_HOME_SCREEN, name) != 0) {
            return 1;
        }
    }

    printf("rendered %zu screens at %ux%u from a %ux%u LVGL canvas\n",
           sizeof(screen_names) / sizeof(screen_names[0]), (unsigned)PANEL_WIDTH,
           (unsigned)PANEL_HEIGHT, (unsigned)JETKVM_SCREEN_SOURCE_WIDTH,
           (unsigned)JETKVM_SCREEN_SOURCE_HEIGHT);
    return 0;
}
