#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "jetkvm_screen_orientation.h"

/*
 * A model of the ST7789 address logic from its datasheet: CASET carries the
 * canvas x plus x_gap, RASET the canvas y plus y_gap; MV exchanges the two,
 * MX reverses the 240 GRAM columns and MY the 320 GRAM rows. The panel shows
 * GRAM rows 0 to 239. The pillar placement is checked against this model, not
 * against its own arithmetic.
 */
typedef struct {
    int column;
    int row;
} panel_pixel_t;

static panel_pixel_t panel_pixel(jetkvm_screen_panel_orientation_t o, int x, int y)
{
    int column_address = x + o.x_gap;
    int page_address = y + o.y_gap;
    int column = o.swap_xy ? page_address : column_address;
    int row = o.swap_xy ? column_address : page_address;
    if (o.mirror_x) column = 239 - column;
    if (o.mirror_y) row = 319 - row;
    return (panel_pixel_t){column, row};
}

static const uint16_t angles[] = {0, 90, 180, 270};

/* Every canvas pixel lands on a distinct visible panel pixel. */
static void test_panel_orientation_covers_the_visible_panel(void)
{
    for (unsigned a = 0; a < 4; a++) {
        jetkvm_screen_panel_orientation_t o = jetkvm_screen_panel_orientation(angles[a]);
        static bool seen[240][240];
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < 240; x++) {
                seen[y][x] = false;
            }
        }
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < 240; x++) {
                panel_pixel_t p = panel_pixel(o, x, y);
                assert(p.column >= 0 && p.column < 240 && p.row >= 0 && p.row < 240);
                assert(!seen[p.row][p.column]);
                seen[p.row][p.column] = true;
            }
        }
    }
}

/* The canvas's top edge runs along the native edge a quarter turn
 * counterclockwise per step, so the picture turns clockwise. */
static void test_each_step_turns_the_picture_clockwise(void)
{
    /* Canvas top-left and top-right corners on the panel. */
    const panel_pixel_t expected[4][2] = {
        {{0, 0}, {239, 0}},       /* 0: top edge along the native top */
        {{239, 0}, {239, 239}},   /* 90: along the native right */
        {{239, 239}, {0, 239}},   /* 180: along the native bottom */
        {{0, 239}, {0, 0}},       /* 270: along the native left */
    };
    for (unsigned a = 0; a < 4; a++) {
        jetkvm_screen_panel_orientation_t o = jetkvm_screen_panel_orientation(angles[a]);
        panel_pixel_t left = panel_pixel(o, 0, 0);
        panel_pixel_t right = panel_pixel(o, 239, 0);
        assert(left.column == expected[a][0].column && left.row == expected[a][0].row);
        assert(right.column == expected[a][1].column && right.row == expected[a][1].row);
    }
}

/* Each pillar part covers the same panel pixels at every angle: beside the
 * native right edge, with the SW3 slot above the SW2 slot. */
static void test_pillar_parts_stay_beside_the_buttons(void)
{
    /* Body (it overhangs two panel edges), the SW3 icon slot and the SW2 icon slot. */
    const int parts[][4] = {{0, 52, 38, 200}, {0, 72, 26, 26}, {0, 198, 26, 26}};
    for (unsigned a = 0; a < 4; a++) {
        jetkvm_screen_panel_orientation_t o = jetkvm_screen_panel_orientation(angles[a]);
        for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
            const int *part = parts[i];
            jetkvm_screen_rect_t r =
                jetkvm_screen_pillar_rect(angles[a], part[0], part[1], part[2], part[3]);
            assert(r.w * r.h == part[2] * part[3]);
            int visible = 0;
            for (int y = r.y; y < r.y + r.h; y++) {
                for (int x = r.x; x < r.x + r.w; x++) {
                    if (x < 0 || x >= 240 || y < 0 || y >= 240) {
                        continue;
                    }
                    panel_pixel_t p = panel_pixel(o, x, y);
                    assert(p.column >= 214 + part[0] && p.column < 214 + part[0] + part[2]);
                    assert(p.row >= part[1] && p.row < part[1] + part[3]);
                    visible++;
                }
            }
            /* The visible part is the same size at every angle. */
            int expected_columns = part[2] < 26 ? part[2] : 26;
            int expected_rows = part[1] + part[3] <= 240 ? part[3] : 240 - part[1];
            assert(visible == expected_columns * expected_rows);
        }
    }
}

/* The canvas edge given for the screen padding is the one the pillar lies on. */
static void test_pillar_edge_matches_the_pillar(void)
{
    for (unsigned a = 0; a < 4; a++) {
        jetkvm_screen_rect_t r = jetkvm_screen_pillar_rect(angles[a], 0, 0, 26, 240);
        switch (jetkvm_screen_pillar_edge(angles[a])) {
        case JETKVM_SCREEN_EDGE_RIGHT:
            assert(r.x == 214 && r.y == 0 && r.w == 26 && r.h == 240);
            break;
        case JETKVM_SCREEN_EDGE_TOP:
            assert(r.x == 0 && r.y == 0 && r.w == 240 && r.h == 26);
            break;
        case JETKVM_SCREEN_EDGE_LEFT:
            assert(r.x == 0 && r.y == 0 && r.w == 26 && r.h == 240);
            break;
        case JETKVM_SCREEN_EDGE_BOTTOM:
            assert(r.x == 0 && r.y == 214 && r.w == 240 && r.h == 26);
            break;
        }
    }
}

/* An upright object keeps its own size and sits centred on its slot. */
static void test_upright_parts_keep_their_size(void)
{
    for (unsigned a = 0; a < 4; a++) {
        jetkvm_screen_rect_t slot = jetkvm_screen_pillar_rect(angles[a], 5, 100, 16, 4);
        jetkvm_screen_rect_t bar = jetkvm_screen_pillar_upright(angles[a], 5, 100, 16, 4);
        assert(bar.w == 16 && bar.h == 4);
        assert(2 * bar.x + bar.w == 2 * slot.x + slot.w);
        assert(2 * bar.y + bar.h == 2 * slot.y + slot.h);
    }
}

/* Back is the button at the top or on the left as the reader sees the
 * screen, Down the one at the bottom or on the right, and each role's icon
 * sits beside the button that has the role. */
static void test_roles_follow_the_reader(void)
{
    const jetkvm_screen_button_role_t sw3_role[4] = {
        JETKVM_SCREEN_ROLE_BACK, JETKVM_SCREEN_ROLE_BACK,
        JETKVM_SCREEN_ROLE_DOWN, JETKVM_SCREEN_ROLE_DOWN,
    };
    for (unsigned a = 0; a < 4; a++) {
        uint16_t d = angles[a];
        jetkvm_screen_button_role_t sw3 = jetkvm_screen_button_role(d, JETKVM_SCREEN_BUTTON_SW3);
        jetkvm_screen_button_role_t sw2 = jetkvm_screen_button_role(d, JETKVM_SCREEN_BUTTON_SW2);
        assert(sw3 == sw3_role[a] && sw2 != sw3);

        /* The physical buttons on the canvas: next to their icon slots. */
        jetkvm_screen_rect_t at_sw3 = jetkvm_screen_pillar_rect(d, 0, 72, 26, 26);
        jetkvm_screen_rect_t at_sw2 = jetkvm_screen_pillar_rect(d, 0, 198, 26, 26);
        jetkvm_screen_rect_t back = sw3 == JETKVM_SCREEN_ROLE_BACK ? at_sw3 : at_sw2;
        jetkvm_screen_rect_t down = sw3 == JETKVM_SCREEN_ROLE_BACK ? at_sw2 : at_sw3;
        jetkvm_screen_edge_t edge = jetkvm_screen_pillar_edge(d);
        if (edge == JETKVM_SCREEN_EDGE_RIGHT || edge == JETKVM_SCREEN_EDGE_LEFT) {
            assert(back.y < down.y);
        } else {
            assert(back.x < down.x);
        }

        /* The Back icon, laid out beside SW3 at 0 degrees, moves to the
         * button that has the role; so do the Down icon and its hint. */
        int back_row = jetkvm_screen_pillar_role_row(d, 76);
        int down_row = jetkvm_screen_pillar_role_row(d, 202);
        int hint_row = jetkvm_screen_pillar_role_row(d, 219);
        assert(back_row == (sw3 == JETKVM_SCREEN_ROLE_BACK ? 76 : 202));
        assert(down_row == (sw3 == JETKVM_SCREEN_ROLE_BACK ? 202 : 76));
        assert(hint_row - down_row == 17);
    }
}

int main(void)
{
    test_panel_orientation_covers_the_visible_panel();
    test_each_step_turns_the_picture_clockwise();
    test_pillar_parts_stay_beside_the_buttons();
    test_pillar_edge_matches_the_pillar();
    test_upright_parts_keep_their_size();
    test_roles_follow_the_reader();
    printf("jetkvm_screen_orientation: all tests passed\n");
    return 0;
}
