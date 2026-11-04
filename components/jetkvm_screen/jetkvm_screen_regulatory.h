#pragma once

#include "lvgl.h"

/*
 * Menu > Regulatory. The generated UI has no such page, so this file builds
 * it with the About page's layout and styles: a "Regulatory" header and a
 * scrolling column with one wrapped paragraph per entry of regulatory.c. It
 * also orders the menu as Set up Wi-Fi (the Wi-Fi board only), Regulatory,
 * Advanced, Status, About, so the page opens from the home screen with Menu,
 * Down and a hold on the Wi-Fi board, and with Menu and a hold on the
 * Ethernet board. All calls run with LVGL owned by the caller.
 */

/* Call once after ui_init(). */
void jetkvm_screen_regulatory_init(void);

/* The menu entry, the page, and its scrolling column. */
lv_obj_t *jetkvm_screen_regulatory_entry(void);
lv_obj_t *jetkvm_screen_regulatory_screen(void);
lv_obj_t *jetkvm_screen_regulatory_list(void);
