/*
 * Every IPv4 address stays inside its line on the home screen: without the
 * button strip, and with the strip at each of the four canvas edges. The
 * check reads the laid-out LVGL objects of the production screens and of
 * the production button code.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "host_ui.h"
#include "jetkvm_screen_buttons.h"
#include "jetkvm_screen_home_address.h"
#include "fonts.h"
#include "screens.h"
#include "ui.h"

static const lv_font_t *const LARGE = &ui_font_font_bold30;
static const lv_font_t *const SMALL = &ui_font_home_address_small;
static int s_failures;

static int32_t text_width(const char *text, const lv_font_t *font)
{
    return lv_text_get_width(text, (uint32_t)strlen(text), font, 0);
}

static int32_t line_width(void)
{
    lv_obj_update_layout(objects.home_screen);
    return lv_obj_get_content_width(lv_obj_get_parent(objects.home_info_ipv4_addr));
}

/* The address that is shown must be drawn whole and inside its line. `font`
 * is the font that it must have, or NULL for either. */
static void check_shown(const char *layout, const lv_font_t *font)
{
    lv_obj_t *label = objects.home_info_ipv4_addr;
    const char *text = lv_label_get_text(label);
    lv_obj_update_layout(objects.home_screen);
    lv_area_t box, line;
    lv_obj_get_coords(label, &box);
    lv_obj_get_content_coords(lv_obj_get_parent(label), &line);
    const lv_font_t *used = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    int32_t width = text_width(text, used);
    const char *size = used == LARGE ? "large" : used == SMALL ? "small" : "another";
    const char *fault = NULL;
    if (used != LARGE && used != SMALL) {
        fault = "is in a font that is not one of the two address fonts";
    } else if (width > lv_area_get_width(&box)) {
        fault = "is wider than its label";
    } else if (box.x1 < line.x1 || box.x2 > line.x2) {
        fault = "crosses the end of its line";
    } else if (font != NULL && used != font) {
        fault = font == LARGE ? "fits in the large font but is in the small one"
                              : "is not in the small font";
    }
    if (fault != NULL) {
        ++s_failures;
        fprintf(stderr, "%s: %s %s (%s font, text %d px, label x %d to %d, line x %d to %d)\n",
                layout, text, fault, size, (int)width, (int)box.x1, (int)box.x2, (int)line.x1,
                (int)line.x2);
    }
}

static void check(const char *layout, const char *text, const lv_font_t *font)
{
    jetkvm_screen_home_address_set(text);
    check_shown(layout, font);
}

/* The addresses of one layout. `widest` is the widest address in the large
 * font and `widest_small` in the small one. */
static void check_layout(const char *layout, const char *widest, const char *widest_small)
{
    /* The large font is used where the address fits it. */
    const lv_font_t *for_widest = text_width(widest, LARGE) <= line_width() ? LARGE : SMALL;
    check(layout, widest, for_widest);
    if (strcmp(widest_small, widest) != 0) {
        check(layout, widest_small, NULL);
    }
    check(layout, "255.255.255.255", NULL);
    /* After a wide address, a narrow one gets the large font again. */
    check(layout, "0.0.0.0", LARGE);
    /* The address of issue #40: complete in the large font on the device. */
    check(layout, "192.168.123.123", LARGE);
}

int main(void)
{
    if (!host_ui_start()) {
        fprintf(stderr, "cannot create LVGL display\n");
        return 1;
    }
    loadScreen(SCREEN_ID_HOME_SCREEN);

    char widest[16], widest_small[16];
    host_ui_widest_address(LARGE, widest);
    host_ui_widest_address(SMALL, widest_small);

    int32_t plain = line_width();
    check_layout("no strip", widest, widest_small);

    host_ui_start_buttons();
    int32_t narrowest = plain;
    static const uint16_t turns[] = {0, 90, 180, 270, 0};
    for (size_t i = 0; i < sizeof(turns) / sizeof(turns[0]); ++i) {
        char layout[32];
        snprintf(layout, sizeof(layout), "strip at %u degrees", (unsigned)turns[i]);
        /* The picture turns while the widest address is shown. */
        jetkvm_screen_home_address_set(widest);
        jetkvm_screen_buttons_set_rotation(turns[i]);
        check_shown(layout, NULL);
        check_layout(layout, widest, widest_small);
        if (line_width() < narrowest) {
            narrowest = line_width();
        }
    }
    /* Without this the checks above did not see the line beside the strip. */
    if (narrowest >= plain) {
        ++s_failures;
        fprintf(stderr, "the button strip did not make the address line narrower than %d px\n",
                (int)plain);
    }
    /* The small font exists for the narrowest line, so it must be needed there. */
    if (text_width(widest, LARGE) <= narrowest) {
        ++s_failures;
        fprintf(stderr, "%s fits %d px in the large font: the small font is not needed\n", widest,
                (int)narrowest);
    }

    printf("address line: %d px, %d px beside the strip; widest address %s: %d px large, "
           "%s: %d px small; 192.168.123.123: %d px large\n",
           (int)plain, (int)narrowest, widest, (int)text_width(widest, LARGE), widest_small,
           (int)text_width(widest_small, SMALL), (int)text_width("192.168.123.123", LARGE));
    if (s_failures != 0) {
        fprintf(stderr, "home address fit: %d failures\n", s_failures);
        return 1;
    }
    printf("home address fit: OK\n");
    return 0;
}
