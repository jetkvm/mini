#include "jetkvm_screen_regulatory.h"

#include <stddef.h>

#include "regulatory.h"
#include "ui/screens.h"
#include "ui/styles.h"

static lv_obj_t *s_entry, *s_screen, *s_list;

/* A transparent container without padding, as the generated pages use. */
static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    return obj;
}

static void opened(lv_event_t *event)
{
    (void)event;
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    lv_screen_load(s_screen);
}

static void entry_init(void)
{
    s_entry = lv_button_create(lv_obj_get_parent(objects.menu_btn_status));
    lv_obj_set_size(s_entry, LV_PCT(100), 50);
    lv_obj_remove_flag(s_entry, LV_OBJ_FLAG_SNAPPABLE);
    add_style_menu_button(s_entry);
    lv_obj_add_event_cb(s_entry, opened, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(s_entry);
    add_style_menu_button_label(label);
    lv_label_set_text(label, "Regulatory");
    /* The menu reads Set up Wi-Fi (the Wi-Fi board only), Regulatory,
     * Advanced, Status, About: Regulatory follows the hidden Access entry,
     * and Status moves down to just above About. */
    lv_obj_move_to_index(s_entry, lv_obj_get_index(objects.menu_btn_access) + 1);
    lv_obj_move_foreground(objects.menu_btn_status);
    lv_obj_move_foreground(objects.menu_btn_about);
}

/* The About page's tree: header row, then a scrolling column of 80 %. */
static void screen_init(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 240, 240);
    add_style_flex_screen_menu(s_screen);

    lv_obj_t *page = box(s_screen);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    add_style_flex_start(page);

    lv_obj_t *header = box(page);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    add_style_flow_row_space_between(header);
    lv_obj_set_style_pad_right(header, 4, LV_PART_MAIN);
    /* The button strip replaces the generated back buttons; the title takes
     * the row's start, as on the other pages. */
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = lv_label_create(header);
    add_style_header_link(title);
    lv_label_set_text(title, "Regulatory");

    s_list = box(page);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(80));
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    add_style_flex_column_start(s_list);
    lv_obj_set_style_pad_right(s_list, 4, LV_PART_MAIN);

    lv_obj_t *column = box(s_list);
    lv_obj_set_size(column, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    add_style_flex_column_start(column);
    lv_obj_set_style_pad_right(column, 10, LV_PART_MAIN);
    for (size_t i = 0; i < jetkvm_regulatory_line_count; i++) {
        lv_obj_t *line = lv_label_create(column);
        lv_obj_set_width(line, LV_PCT(100));
        add_style_info_content_label(line);
        lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
        lv_label_set_text_static(line, jetkvm_regulatory_lines[i]);
    }
}

void jetkvm_screen_regulatory_init(void)
{
    if (s_screen != NULL) {
        return;
    }
    screen_init();
    entry_init();
}

lv_obj_t *jetkvm_screen_regulatory_entry(void)
{
    return s_entry;
}

lv_obj_t *jetkvm_screen_regulatory_screen(void)
{
    return s_screen;
}

lv_obj_t *jetkvm_screen_regulatory_list(void)
{
    return s_list;
}
