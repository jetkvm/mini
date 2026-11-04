#include "jetkvm_screen_orientation.h"

#define PANEL_SIZE 240
/* The pillar's inner side at 0 degrees. */
#define PILLAR_X (PANEL_SIZE - 26)
/* Native rows of the SW3 and SW2 icon slots; role parts sit near them. */
#define SW3_SLOT_ROW 76
#define SW2_SLOT_ROW 202
#define SLOT_MIDDLE ((SW3_SLOT_ROW + SW2_SLOT_ROW) / 2)

jetkvm_screen_panel_orientation_t jetkvm_screen_panel_orientation(uint16_t degrees)
{
    /* The ST7789's GRAM has 320 rows; the panel shows rows 0 to 239, so a
     * mirrored row order starts 80 rows later. */
    return (jetkvm_screen_panel_orientation_t){
        .swap_xy = degrees == 90 || degrees == 270,
        .mirror_x = degrees == 90 || degrees == 180,
        .mirror_y = degrees == 180 || degrees == 270,
        .x_gap = degrees == 270 ? 80 : 0,
        .y_gap = degrees == 180 ? 80 : 0,
    };
}

jetkvm_screen_edge_t jetkvm_screen_pillar_edge(uint16_t degrees)
{
    switch (degrees) {
    case 90:
        return JETKVM_SCREEN_EDGE_TOP;
    case 180:
        return JETKVM_SCREEN_EDGE_LEFT;
    case 270:
        return JETKVM_SCREEN_EDGE_BOTTOM;
    default:
        return JETKVM_SCREEN_EDGE_RIGHT;
    }
}

jetkvm_screen_rect_t jetkvm_screen_pillar_rect(uint16_t degrees, int u, int s, int w, int h)
{
    /* Native panel column x = PILLAR_X + u and row y = s, turned by the
     * same quarter turn as the picture. */
    int x = PILLAR_X + u;
    switch (degrees) {
    case 90:
        return (jetkvm_screen_rect_t){s, PANEL_SIZE - x - w, h, w};
    case 180:
        return (jetkvm_screen_rect_t){PANEL_SIZE - x - w, PANEL_SIZE - s - h, w, h};
    case 270:
        return (jetkvm_screen_rect_t){PANEL_SIZE - s - h, x, h, w};
    default:
        return (jetkvm_screen_rect_t){x, s, w, h};
    }
}

jetkvm_screen_rect_t jetkvm_screen_pillar_upright(uint16_t degrees, int u, int s, int w, int h)
{
    jetkvm_screen_rect_t area = jetkvm_screen_pillar_rect(degrees, u, s, w, h);
    return (jetkvm_screen_rect_t){area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h};
}

static bool roles_swapped(uint16_t degrees)
{
    return degrees == 180 || degrees == 270;
}

jetkvm_screen_button_role_t jetkvm_screen_button_role(uint16_t degrees, jetkvm_screen_button_id_t button)
{
    bool back = (button == JETKVM_SCREEN_BUTTON_SW3) != roles_swapped(degrees);
    return back ? JETKVM_SCREEN_ROLE_BACK : JETKVM_SCREEN_ROLE_DOWN;
}

int jetkvm_screen_pillar_role_row(uint16_t degrees, int row)
{
    if (!roles_swapped(degrees)) {
        return row;
    }
    int distance = SW2_SLOT_ROW - SW3_SLOT_ROW;
    return row < SLOT_MIDDLE ? row + distance : row - distance;
}
