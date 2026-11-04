#!/usr/bin/env python3
"""Run the production band worker against connection and persistence failures."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/'jetkvm_c5_wifi.c').read_text()
a=source.index('static esp_err_t c5_wait_station_connected(')
b=source.index('\nconst char *jetkvm_c5_wifi_set_band',a)
stub=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int jetkvm_wifi_band_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_NOT_FOUND 2
#define C5_BAND_SEARCH_LIMIT_MS 30000U
#define C5_CONNECT_ACK_MS 3000U
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGE(...) ((void)0)
static struct {int band;bool applying,apply_failed;} s_wifi_settings;
static int s_previous_band;
static bool s_station_connected;
static unsigned s_band_requested, s_band_applied, s_band_searched, stale_restart;
static int starts,saves,saved,fail_start,fail_save,connect_after,search_ends_after,ticks;
static void ulTaskNotifyTake(int a,int b) {(void)a;(void)b;}
static void vTaskDelay(int ms) {
 if(ms!=100)return;
 ++ticks;
 if(ticks==connect_after)s_station_connected=true;
 /* The connection task tried every candidate on the new band. */
 if(ticks==search_ends_after)s_band_searched=s_band_requested;
}
static void vTaskDelete(void *p) {(void)p;}
/* The connection task has not yet served the request when stale_restart is set. */
static int c5_restart_selected_band(void) {starts++;s_station_connected=false;s_band_requested++;if(!stale_restart)s_band_applied=s_band_requested;return starts==1?fail_start:0;}
static int c5_save_band(int band) {saves++;if(fail_save)return 9;saved=band;return 0;}
static void c5_set_band_result(bool failed) {s_wifi_settings.applying=false;s_wifi_settings.apply_failed=failed;}
static void reset(void) {stale_restart=0;s_band_requested=s_band_applied=s_band_searched=0;search_ends_after=0;s_wifi_settings.band=2;s_wifi_settings.applying=true;s_wifi_settings.apply_failed=false;s_previous_band=saved=1;s_station_connected=false;starts=saves=fail_start=fail_save=connect_after=ticks=0;}
'''
test=r'''
int main(void) {
 reset();connect_after=4;c5_band_worker(NULL);
 assert(starts==1 && saves==1 && saved==2 && !s_wifi_settings.apply_failed && !s_wifi_settings.applying);
 reset();c5_band_worker(NULL);
 assert(ticks==300 && starts==2 && saves==0 && saved==1 && s_wifi_settings.band==1 && s_wifi_settings.apply_failed && !s_wifi_settings.applying);
 /* A search that ends without a connection rolls back at once... */
 reset();search_ends_after=5;c5_band_worker(NULL);
 assert(ticks==5 && starts==2 && saves==0 && s_wifi_settings.band==1 && s_wifi_settings.apply_failed);
 /* ...and a slow search that joins late still confirms the band. */
 reset();connect_after=250;c5_band_worker(NULL);
 assert(starts==1 && saves==1 && saved==2 && !s_wifi_settings.apply_failed);
 reset();fail_start=8;c5_band_worker(NULL);
 assert(starts==2 && saves==0 && saved==1 && s_wifi_settings.band==1 && s_wifi_settings.apply_failed);
 /* A connection from before the restart finished does not confirm the band. */
 reset();connect_after=2;stale_restart=1;c5_band_worker(NULL);
 assert(saves==0 && saved==1 && s_wifi_settings.band==1 && s_wifi_settings.apply_failed);
 reset();connect_after=2;fail_save=1;c5_band_worker(NULL);
 assert(starts==2 && saves==1 && saved==1 && s_wifi_settings.band==1 && s_wifi_settings.apply_failed);
 puts("C5 band connection timeout / persistence rollback: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.c').write_text(stub+source[a:b]+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
