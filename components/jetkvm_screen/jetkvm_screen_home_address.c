#include "jetkvm_screen_home_address.h"

#include <string.h>

#include "ui/fonts.h"
#include "ui/screens.h"

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
