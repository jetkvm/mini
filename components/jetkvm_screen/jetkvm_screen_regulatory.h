#pragma once

#include "lvgl.h"

/*
 * Menu > Regulatory, on the Wi-Fi board only. The generated UI has no such
 * page, so this file builds it with the About page's layout and styles: a
 * "Regulatory" header and a scrolling column with one wrapped paragraph per
 * entry of regulatory.c. The page opens from the home screen with Menu, Down
 * and a hold.
 *
 * This file also orders the menu, on both boards: Set up Wi-Fi and
 * Regulatory (the Wi-Fi board only), Advanced, Status, About. All calls run
 * with LVGL owned by the caller.
 */

/* Call once after ui_init(). */
void jetkvm_screen_regulatory_init(void);

/* The menu entry, the page, and its scrolling column; NULL on the Ethernet
 * board, which has no such page. */
lv_obj_t *jetkvm_screen_regulatory_entry(void);
lv_obj_t *jetkvm_screen_regulatory_screen(void);
lv_obj_t *jetkvm_screen_regulatory_list(void);
