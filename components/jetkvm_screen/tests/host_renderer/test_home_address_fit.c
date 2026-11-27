/*
 * Every IPv4 address stays inside its line on the home screen: without the
 * button strip, and with the strip at each of the four canvas edges. So does
 * every IPv6 address on its two lines, which are as equal in width as a
 * break after a colon makes them. The check reads the laid-out LVGL objects
 * of the production screens and of the production button code.
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

/* The IPv6 address with the widest digit in each place: no address is wider. */
static void widest_ipv6(const lv_font_t *font, char address[40])
{
    char widest = '0';
    int32_t width = 0;
    for (const char *digit = "0123456789abcdef"; *digit; ++digit) {
        int32_t one = lv_text_get_width(digit, 1U, font, 0);
        if (one > width) {
            width = one;
            widest = *digit;
        }
    }
    char *out = address;
    for (unsigned group = 0; group < 8U; ++group) {
        if (group) *out++ = ':';
        memset(out, widest, 4U);
        out += 4;
    }
    *out = '\0';
}

static void ipv6_fault(const char *layout, const char *text, const char *fault)
{
    ++s_failures;
    fprintf(stderr, "%s: %s %s\n", layout, text, fault);
}

/* An IPv6 address is drawn whole on at most two lines, inside its line and
 * above the end of the address rows; the MAC address has that room only
 * without an IPv6 address. */
static void check_ipv6(const char *layout)
{
    lv_obj_t *label = objects.home_info_ipv6_addr;
    lv_obj_t *mac = objects.home_info_mac_addr;
    lv_obj_t *rows = lv_obj_get_parent(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    char widest[40];
    widest_ipv6(font, widest);
    const char *const addresses[] = {widest, "2001:db8:85a3:8d3:1319:8a2e:370:7348", "fe80::1"};
    lv_area_t box, line;
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        jetkvm_screen_home_ipv6_set(addresses[i]);
        lv_obj_update_layout(objects.home_screen);
        lv_obj_get_coords(label, &box);
        lv_obj_get_content_coords(rows, &line);
        /* What LVGL draws of the text in the label's width, wrapped. */
        lv_point_t drawn;
        lv_text_get_size(&drawn, addresses[i], font, 0,
                         lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                         lv_area_get_width(&box), LV_TEXT_FLAG_NONE);
        int32_t two_lines = 2 * lv_font_get_line_height(font) +
                            lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
        if (lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) {
            ipv6_fault(layout, addresses[i], "is not shown");
        } else if (drawn.x > lv_area_get_width(&box) || drawn.y > lv_area_get_height(&box)) {
            ipv6_fault(layout, addresses[i], "is not drawn whole in its label");
        } else if (drawn.y > two_lines) {
            ipv6_fault(layout, addresses[i], "takes more than two lines");
        } else if (box.x1 < line.x1 || box.x2 > line.x2) {
            ipv6_fault(layout, addresses[i], "crosses the end of its line");
        } else if (box.y2 > line.y2) {
            ipv6_fault(layout, addresses[i], "ends below the address rows");
        } else if (!lv_obj_has_flag(mac, LV_OBJ_FLAG_HIDDEN)) {
            ipv6_fault(layout, addresses[i], "is shown together with the MAC address, which has no room");
        }
    }
    jetkvm_screen_home_ipv6_set("");
    lv_obj_update_layout(objects.home_screen);
    lv_obj_get_coords(mac, &box);
    lv_obj_get_content_coords(rows, &line);
    if (!lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) {
        ipv6_fault(layout, "no IPv6 address", "still shows the IPv6 line");
    } else if (lv_obj_has_flag(mac, LV_OBJ_FLAG_HIDDEN) || box.y2 > line.y2) {
        ipv6_fault(layout, "no IPv6 address", "does not show the MAC address in the address rows");
    }
}

#define DRAWN_LINES 3U

/* The lines that LVGL draws of the label's text, read from the position of
 * each letter. Returns their number; only the first DRAWN_LINES are kept. */
static size_t drawn_lines(lv_obj_t *label, char lines[DRAWN_LINES][64])
{
    const char *text = lv_label_get_text(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    int32_t pitch = lv_font_get_line_height(font) +
                    lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    size_t count = 0;
    size_t used[DRAWN_LINES] = {0};
    for (size_t i = 0; i < DRAWN_LINES; ++i) lines[i][0] = '\0';
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n') continue;
        lv_point_t at;
        lv_label_get_letter_pos(label, i, &at);
        size_t line = (size_t)(at.y / pitch);
        if (line + 1U > count) count = line + 1U;
        if (line < DRAWN_LINES && used[line] + 1U < sizeof(lines[line])) {
            lines[line][used[line]++] = text[i];
            lines[line][used[line]] = '\0';
        }
    }
    return count;
}

/* An address that does not fit one line breaks after the colon that makes
 * the two lines most equal in drawn width, in each layout. A first line that
 * is filled to its end, as LVGL wraps a text, is a different break for each
 * of these addresses. */
typedef struct {
    const char *address;
    const char *first;
    const char *second;
} ipv6_break_t;
static const ipv6_break_t IPV6_BREAKS[] = {
    /* Eight groups: four on each line. */
    {"2001:db8:1234:5678:3252:53ff:fe1b:c5f5", "2001:db8:1234:5678:", "3252:53ff:fe1b:c5f5"},
    {"2001:db8:85a3:8d3a:1319:8a2e:3707:7348", "2001:db8:85a3:8d3a:", "1319:8a2e:3707:7348"},
    /* Short groups first: the break is after the fifth group. */
    {"2001:db8:1:2:200:5eff:fe00:5310", "2001:db8:1:2:200:", "5eff:fe00:5310"},
    /* "::" stays whole. A break between its two colons makes the lines more
     * equal, and the second line would start with a colon. */
    {"2001:db8:85a3::8a2e:370:7348", "2001:db8:85a3::", "8a2e:370:7348"},
    /* The breaks before and after "2222:" are equally uneven (see main()):
     * the longer line is the first one. */
    {"1111:1111:1111:2222:1111:1111::1111", "1111:1111:1111:2222:", "1111:1111::1111"},
};

/* `report` prints the widths of the lines. */
static void check_ipv6_break(const char *layout, const ipv6_break_t *want, bool report)
{
    lv_obj_t *label = objects.home_info_ipv6_addr;
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    jetkvm_screen_home_ipv6_set(want->address);
    lv_obj_update_layout(objects.home_screen);
    int32_t room = lv_obj_get_content_width(label);
    /* An address that fits one line stays on one line. */
    bool one_line = text_width(want->address, font) <= room;
    const char *first = one_line ? want->address : want->first;
    const char *second = one_line ? "" : want->second;
    char lines[DRAWN_LINES][64];
    size_t count = drawn_lines(label, lines);
    if (count != (one_line ? 1U : 2U) || strcmp(lines[0], first) != 0 ||
        strcmp(lines[1], second) != 0) {
        ++s_failures;
        fprintf(stderr, "%s: %s is drawn on %u lines as \"%s\" (%d px), \"%s\" (%d px) in %d px, "
                "and not as \"%s\", \"%s\"\n", layout, want->address, (unsigned)count, lines[0],
                (int)text_width(lines[0], font), lines[1], (int)text_width(lines[1], font),
                (int)room, first, second);
    } else if (report && one_line) {
        printf("%s: %s is one line of %d px in %d px\n", layout, want->address,
               (int)text_width(lines[0], font), (int)room);
    } else if (report) {
        printf("%s: %s is two lines of %d px and %d px in %d px\n", layout, want->address,
               (int)text_width(lines[0], font), (int)text_width(lines[1], font), (int)room);
    }
}

static void check_ipv6_breaks(const char *layout, bool report)
{
    for (size_t i = 0; i < sizeof(IPV6_BREAKS) / sizeof(IPV6_BREAKS[0]); ++i) {
        check_ipv6_break(layout, &IPV6_BREAKS[i], report);
    }
    /* The widest address has four groups on each line, so every address has
     * a break with two lines that fit. */
    char widest[40], first[21], second[20];
    widest_ipv6(lv_obj_get_style_text_font(objects.home_info_ipv6_addr, LV_PART_MAIN), widest);
    memcpy(first, widest, 20U);
    first[20] = '\0';
    memcpy(second, widest + 20, 20U);
    check_ipv6_break(layout, &(ipv6_break_t){widest, first, second}, report);
    /* A link-local address from a MAC address fits one line. */
    check_ipv6_break(layout, &(ipv6_break_t){"fe80::200:5eff:fe00:5310", "", ""}, report);
    jetkvm_screen_home_ipv6_set("");
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
    check_ipv6("no strip");
    check_ipv6_breaks("no strip", true);

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
        /* Beside the widest IPv4 address in its font of this layout. */
        jetkvm_screen_home_address_set(widest);
        check_ipv6(layout);
        check_ipv6_breaks(layout, i == 0);
        if (line_width() < narrowest) {
            narrowest = line_width();
        }
    }
    /* The tie of IPV6_BREAKS holds in the label's font. */
    const lv_font_t *ipv6_font =
        lv_obj_get_style_text_font(objects.home_info_ipv6_addr, LV_PART_MAIN);
    if (text_width("1111:1111:1111:", ipv6_font) != text_width("1111:1111::1111", ipv6_font)) {
        ++s_failures;
        fprintf(stderr, "\"1111:1111:1111:\" and \"1111:1111::1111\" differ in width in the "
                "IPv6 font: the address with \"2222:\" between them has no tie\n");
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
