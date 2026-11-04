#!/usr/bin/env python3
"""Execute the production rotation callback and LVGL-owner redraw handoff,
including the move of the button strip to the new angle."""
from pathlib import Path
import subprocess,tempfile
component=Path(__file__).resolve().parents[2]
source=(component/'jetkvm_screen.c').read_text()
start=source.index('static esp_err_t screen_display_rotation(')
end=source.index('static esp_err_t screen_display_brightness(',start)
functions=source[start:end]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "jetkvm_screen_orientation.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define SCREEN_PANEL_ROTATION_DEGREES 0
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define ESP_LOGI(...) ((void)0)
static struct {int preview_mutex,panel;bool preview_layout_valid,ui_redraw_requested;uint16_t ui_degrees;} s_screen={1,2,true,false,0};
static bool held,owner;
static int panel_error,invalidations;
static int xSemaphoreTake(int p,int ticks) {(void)p;(void)ticks;assert(!held);held=true;return 1;}
static void xSemaphoreGive(int p) {(void)p;assert(held);held=false;}
static int screen_wait_for_transfer(void *s,int ticks) {(void)s;(void)ticks;return 0;}
static int esp_lcd_panel_swap_xy(int p,bool swap) {(void)p;(void)swap;assert(held);return panel_error;}
static int esp_lcd_panel_mirror(int p,bool x,bool y) {(void)p;(void)x;(void)y;return panel_error;}
static int esp_lcd_panel_set_gap(int p,int x,int y) {(void)p;(void)x;(void)y;return panel_error;}
static int lv_screen_active(void) {assert(owner&&held);return 1;}
static void lv_obj_invalidate(int screen) {assert(screen==1&&owner&&held);++invalidations;}
static int strip_degrees=-1;
static void jetkvm_screen_buttons_set_rotation(uint16_t degrees) {assert(owner&&held);strip_degrees=degrees;}
'''
main=r'''
int main(void) {
 for(unsigned rotation=0;rotation<360;rotation+=90) {
  int before=invalidations;owner=false;
  assert(screen_display_rotation(rotation)==0);
  assert(s_screen.ui_redraw_requested&&!s_screen.preview_layout_valid&&invalidations==before);
  owner=true;held=true;screen_apply_pending_redraw();
  assert(invalidations==before+1&&!s_screen.ui_redraw_requested);
  assert(strip_degrees==(int)((rotation+90)%360));
  screen_apply_pending_redraw();assert(invalidations==before+1);held=false;
 }
 panel_error=1;owner=false;
 assert(screen_display_rotation(0)!=0&&!s_screen.ui_redraw_requested);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(prefix+functions+main)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(component),str(p/'test.c'),str(component/'jetkvm_screen_orientation.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('All rotations request one full redraw and a strip move on the LVGL owner: PASS')
