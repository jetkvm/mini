#!/usr/bin/env python3
"""Exercise real C5 band transition code with delayed remote stop events."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/'jetkvm_c5_wifi.c').read_text()
def function(signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
stub=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int wifi_band_mode_t;
typedef int wifi_band_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define WIFI_BAND_MODE_AUTO 0
#define WIFI_BAND_MODE_2G_ONLY 1
#define WIFI_BAND_MODE_5G_ONLY 2
#define WIFI_BAND_2G 1
#define WIFI_BAND_5G 2
#define pdMS_TO_TICKS(x) (x)
static bool s_station_stopped;
static int desired, current, physical, delay, starts, fail;
static bool switches;
static wifi_band_mode_t c5_selected_band(void) {return desired;}
static int esp_wifi_get_band_mode(int *v) {*v=current;return 0;}
static int esp_wifi_get_band(int *v) {*v=physical;return 0;}
static int esp_wifi_set_band_mode(int v) {current=v;return fail;}
static void vTaskDelay(int ms) {(void)ms;if(switches && --delay==0)s_station_stopped=true;}
static int esp_wifi_start(void) {assert(s_station_stopped);starts++;return 0;}
static int c5_station_start(void) {return esp_wifi_start();}
'''
test=r'''
int main(void) {
 current=WIFI_BAND_MODE_AUTO;physical=WIFI_BAND_2G;desired=WIFI_BAND_MODE_2G_ONLY;
 assert(c5_apply_band()==ESP_OK);assert(starts==0);
 desired=WIFI_BAND_MODE_5G_ONLY;switches=true;delay=8;
 assert(c5_apply_band()==ESP_OK);assert(starts==1 && delay==0);
 physical=WIFI_BAND_5G;desired=WIFI_BAND_MODE_2G_ONLY;delay=5;
 assert(c5_apply_band()==ESP_OK);assert(starts==2);
 physical=WIFI_BAND_2G;desired=WIFI_BAND_MODE_5G_ONLY;switches=false;
 assert(c5_apply_band()==ESP_ERR_TIMEOUT);assert(starts==2);
 current=WIFI_BAND_MODE_AUTO;fail=17;
 assert(c5_apply_band()==17);assert(starts==2);
 puts("C5 band switch / delayed stop event: PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(stub+function('static esp_err_t c5_wait_station_stopped(')+function('static esp_err_t c5_apply_band(')+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
