#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The local display turns the picture with the ST7789 MADCTL bits; LVGL keeps
 * drawing an upright 240x240 canvas. `degrees` is the clockwise turn of the
 * picture against the panel's native orientation, one of 0, 90, 180 or 270.
 */
typedef struct {
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    int x_gap;
    int y_gap;
} jetkvm_screen_panel_orientation_t;

jetkvm_screen_panel_orientation_t jetkvm_screen_panel_orientation(uint16_t degrees);

/*
 * The button pillar is fixed beside the panel's native right edge, where the
 * two push buttons are. Pillar parts are described in that frame: `u` runs
 * across the 26 px column from its inner side and `s` runs along the edge
 * from the native top. The functions return where a part lands on the LVGL
 * canvas when the picture is turned by `degrees`.
 */
typedef enum {
    JETKVM_SCREEN_EDGE_RIGHT,
    JETKVM_SCREEN_EDGE_TOP,
    JETKVM_SCREEN_EDGE_LEFT,
    JETKVM_SCREEN_EDGE_BOTTOM,
} jetkvm_screen_edge_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} jetkvm_screen_rect_t;

/* The canvas edge that lies along the buttons. */
jetkvm_screen_edge_t jetkvm_screen_pillar_edge(uint16_t degrees);

/* The canvas rectangle covered by the pillar rectangle (u, s, w, h). */
jetkvm_screen_rect_t jetkvm_screen_pillar_rect(uint16_t degrees, int u, int s, int w, int h);

/* An upright w x h object centred where the pillar rectangle (u, s, w, h)
 * lands, so text and icons read the right way up at every angle. */
jetkvm_screen_rect_t jetkvm_screen_pillar_upright(uint16_t degrees, int u, int s, int w, int h);

/*
 * Button roles follow the reader: Back is the button at the top or on the
 * left as the reader sees the screen, and Down (hold to accept) the one at
 * the bottom or on the right. At 0 and 90 degrees that is SW3 for Back; at
 * 180 and 270 degrees the roles swap and SW2 is Back.
 */
typedef enum {
    JETKVM_SCREEN_BUTTON_SW3,
    JETKVM_SCREEN_BUTTON_SW2,
} jetkvm_screen_button_id_t;

typedef enum {
    JETKVM_SCREEN_ROLE_BACK,
    JETKVM_SCREEN_ROLE_DOWN,
} jetkvm_screen_button_role_t;

jetkvm_screen_button_role_t jetkvm_screen_button_role(uint16_t degrees, jetkvm_screen_button_id_t button);

/* The native row of a pillar part that belongs to a role: parts are laid out
 * beside SW3 for Back and beside SW2 for Down at 0 degrees, and move by the
 * distance between the two buttons' slots when the roles swap. */
int jetkvm_screen_pillar_role_row(uint16_t degrees, int row);

#ifdef __cplusplus
}
#endif
