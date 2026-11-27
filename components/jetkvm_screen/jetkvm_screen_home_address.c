#include "jetkvm_screen_home_address.h"

#include <string.h>

#include "ui/fonts.h"
#include "ui/screens.h"

/* An IPv6 address as text (45 bytes at most), with its terminator. */
#define IPV6_CAPACITY 46U

/*
 * Where an address breaks into two lines: the number of bytes of the first
 * line, which ends in a colon. Of the breaks after a colon whose two lines
 * both fit `width`, it is the one with the smallest difference between the
 * drawn widths of the lines. Of two breaks with the same difference, the one
 * with the longer line first wins. 0 when no break makes both lines fit.
 */
static size_t even_break(const char *text, const lv_font_t *font, int32_t letter_space,
                         int32_t width)
{
    size_t length = strlen(text);
    size_t best = 0;
    int32_t smallest = INT32_MAX;
    for (size_t first_length = 1; first_length < length; ++first_length) {
        if (text[first_length - 1U] != ':') {
            continue;
        }
        int32_t first = lv_text_get_width(text, (uint32_t)first_length, font, letter_space);
        int32_t second = lv_text_get_width(text + first_length, (uint32_t)(length - first_length),
                                           font, letter_space);
        int32_t difference = first > second ? first - second : second - first;
        /* A later break has a longer first line, so it wins a tie. */
        if (first <= width && second <= width && difference <= smallest) {
            smallest = difference;
            best = first_length;
        }
    }
    return best;
}

/* Breaks the IPv6 address that is shown for the width that its label has. */
static void ipv6_fit(void)
{
    lv_obj_t *label = objects.home_info_ipv6_addr;
    if (label == NULL || lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    /* The address without the break of an earlier fit. A text that is longer
     * than an address stays as it is. */
    char address[IPV6_CAPACITY];
    size_t length = 0;
    for (const char *shown = lv_label_get_text(label); *shown != '\0'; ++shown) {
        if (*shown == '\n') {
            continue;
        }
        if (length + 1U >= sizeof(address)) {
            return;
        }
        address[length++] = *shown;
    }
    address[length] = '\0';

    lv_obj_update_layout(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    int32_t width = lv_obj_get_content_width(label);
    char lines[IPV6_CAPACITY + 1U];
    size_t first_length = 0;
    if (lv_text_get_width(address, (uint32_t)length, font, letter_space) > width) {
        first_length = even_break(address, font, letter_space, width);
    }
    /* Without a break LVGL wraps the address where a line is full. */
    if (first_length != 0) {
        memcpy(lines, address, first_length);
        lines[first_length] = '\n';
        memcpy(lines + first_length + 1U, address + first_length, length - first_length + 1U);
    } else {
        memcpy(lines, address, length + 1U);
    }
    if (strcmp(lv_label_get_text(label), lines) != 0) {
        lv_label_set_text(label, lines);
    }
}

void jetkvm_screen_home_address_fit(void)
{
    lv_obj_t *label = objects.home_info_ipv4_addr;
    if (label == NULL) {
        return;
    }
    /* The line's width follows the screen's padding and the line's margin,
     * which the button strip changes when the picture turns. */
    lv_obj_t *line = lv_obj_get_parent(label);
    lv_obj_update_layout(line);
    const char *text = lv_label_get_text(label);
    const lv_font_t *font = &ui_font_font_bold30;
    int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    if (lv_text_get_width(text, (uint32_t)strlen(text), font, letter_space) >
        lv_obj_get_content_width(line)) {
        font = &ui_font_home_address_small;
    }
    if (lv_obj_get_style_text_font(label, LV_PART_MAIN) != font) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    ipv6_fit();
}

/* The label shows `address`, on one line or broken into two. */
static bool ipv6_shows(lv_obj_t *label, const char *address)
{
    const char *shown = lv_label_get_text(label);
    while (*shown != '\0') {
        if (*shown == '\n') {
            ++shown;
        } else if (*shown++ != *address++) {
            return false;
        }
    }
    return *address == '\0';
}

void jetkvm_screen_home_ipv6_set(const char *text)
{
    lv_obj_t *label = objects.home_info_ipv6_addr;
    lv_obj_t *mac = objects.home_info_mac_addr;
    if (label == NULL || mac == NULL) {
        return;
    }
    bool shown = text != NULL && text[0] != '\0';
    if (shown) {
        /* The generated label is one line that ends in dots. */
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        if (!ipv6_shows(label, text)) {
            lv_label_set_text(label, text);
        }
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mac, LV_OBJ_FLAG_HIDDEN);
        ipv6_fit();
    } else {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(mac, LV_OBJ_FLAG_HIDDEN);
    }
}

void jetkvm_screen_home_address_set(const char *text)
{
    lv_obj_t *label = objects.home_info_ipv4_addr;
    if (label == NULL || text == NULL || strcmp(lv_label_get_text(label), text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
    jetkvm_screen_home_address_fit();
}
