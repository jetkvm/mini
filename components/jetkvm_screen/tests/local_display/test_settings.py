#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4]
source=(root/'components/jetkvm_screen/local_display_settings.c').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t,nvs_handle_t,portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef void *SemaphoreHandle_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_NVS_NOT_FOUND 5
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define ESP_LOGW(tag,...) ((void)(tag))
typedef struct { uint32_t max_brightness,dim_after,off_after; } jetkvm_backlight_settings_t;
typedef struct {
 bool (*get_backlight)(jetkvm_backlight_settings_t *,void *);
 const char *(*set_backlight)(const jetkvm_backlight_settings_t *,void *);
 bool (*get_rotation)(uint16_t *,void *);
 const char *(*set_rotation)(uint16_t,void *);
 void *ctx;
} jetkvm_local_display_context_t;
typedef struct { int (*rotation)(uint16_t);int (*brightness)(uint32_t);int (*active)(bool); } local_display_driver_t;
typedef struct { bool ready,active,dark;uint16_t rotation;uint32_t brightness;int error; } local_display_state_t;
static jetkvm_local_display_context_t registered;
static bool held,has_saved,save_fails;
static uint32_t saved[5];
static int64_t now;
static int rotation_calls,brightness_calls,rotation_fail_at,brightness_fail_at,active_calls,active_fail_at;
static uint16_t physical_rotation;
static uint32_t physical_brightness;
static bool physical_active;
static void jetkvm_control_set_default_local_display_context(const jetkvm_local_display_context_t *ctx) { registered=*ctx; }
static void *xSemaphoreCreateMutex(void) { return (void *)1; }
static int xSemaphoreTake(void *p,unsigned timeout) { (void)p;(void)timeout;if(held)return 0;held=true;return 1; }
static void xSemaphoreGive(void *p) { (void)p;assert(held);held=false; }
static int64_t esp_timer_get_time(void) { return now; }
static int nvs_open(const char *ns,int mode,int *h) { (void)mode;assert(!strcmp(ns,"jk_display"));*h=1;return 0; }
static void nvs_close(int h) { assert(h==1); }
static int nvs_get_blob(int h,const char *key,void *p,size_t *n) {
 assert(h==1&&!strcmp(key,"settings")&&*n==sizeof(saved));if(!has_saved)return ESP_ERR_NVS_NOT_FOUND;
 memcpy(p,saved,*n);return 0;
}
static int nvs_set_blob(int h,const char *key,const void *p,size_t n) {
 assert(h==1&&!strcmp(key,"settings")&&n==sizeof(saved));if(save_fails)return 1;
 memcpy(saved,p,n);has_saved=true;return 0;
}
static int nvs_commit(int h) { assert(h==1);return 0; }
static int rotate(uint16_t degrees) { physical_rotation=degrees;return ++rotation_calls==rotation_fail_at; }
static int light(uint32_t value) { physical_brightness=value;return ++brightness_calls==brightness_fail_at; }
static int active(bool enabled) { physical_active=enabled;return ++active_calls==active_fail_at; }
'''
main=r'''
int main(void) {
 const local_display_driver_t driver={rotate,light,active};
 assert(!local_display_settings_init(270,&driver,true));
 jetkvm_backlight_settings_t bl;
 assert(!registered.get_backlight(&bl,NULL));
 assert(!local_display_prepare()&&!local_display_ready()&&!local_display_active(true));
 assert(physical_active&&physical_rotation==270&&physical_brightness==35);
 local_display_state_t snapshot;
 assert(local_display_state(&snapshot)&&snapshot.brightness==35);
 held=true;s_state.brightness=123; /* Simulate a setter blocked in NVS/SPI. */
 assert(local_display_state(&snapshot)&&snapshot.brightness==35&&held);
 s_state.brightness=35;unlock();

 bl=(jetkvm_backlight_settings_t){64,2,4};assert(!registered.set_backlight(&bl,NULL));
 now=1999999;local_display_tick();assert(physical_brightness==64);
 assert(local_display_state(&snapshot)&&!snapshot.dark);
 now=2000000;local_display_tick();assert(physical_brightness==32);
 /* Dimmed is not dark: the panel still shows its picture. */
 assert(local_display_state(&snapshot)&&!snapshot.dark);
 now=4000000;local_display_tick();assert(physical_brightness==0);
 assert(local_display_state(&snapshot)&&snapshot.dark);
 local_display_wake();local_display_tick();assert(physical_brightness==64);
 assert(local_display_state(&snapshot)&&!snapshot.dark);
 now=8000000;local_display_tick();assert(physical_brightness==0);
 assert(!registered.set_rotation(90,NULL)&&physical_rotation==90&&physical_brightness==64);
 now=9999999;local_display_tick();assert(physical_brightness==64);
 now=10000000;local_display_tick();assert(physical_brightness==32);
 brightness_fail_at=brightness_calls+1;
 assert(registered.set_rotation(180,NULL)&&physical_rotation==90&&physical_brightness==32);
 local_display_wake();local_display_tick();assert(physical_brightness==64);
 save_fails=true;assert(registered.set_rotation(180,NULL)&&physical_rotation==90);
 bl.max_brightness=10;assert(registered.set_backlight(&bl,NULL)&&physical_brightness==64);
 save_fails=false;rotation_fail_at=rotation_calls+1;assert(registered.set_rotation(0,NULL)&&physical_rotation==90);
 brightness_fail_at=brightness_calls+1;assert(registered.set_backlight(&bl,NULL)&&physical_brightness==64);
 /* A set brightness of 0 is the setting, not the timers. */
 bl=(jetkvm_backlight_settings_t){0,0,0};assert(!registered.set_backlight(&bl,NULL)&&!physical_brightness);
 assert(local_display_state(&snapshot)&&!snapshot.dark);
 bl=(jetkvm_backlight_settings_t){255,0,0};assert(!registered.set_backlight(&bl,NULL));
 now=INT64_MAX;local_display_tick();assert(physical_brightness==255);
 bl=(jetkvm_backlight_settings_t){35,8,3};assert(local_display_brightness(&bl,3)==0);
 bl.max_brightness=256;assert(registered.set_backlight(&bl,NULL));
 assert(registered.set_rotation(45,NULL));
 active_fail_at=active_calls+1;
 assert(local_display_active(false)&&physical_active&&s_state.active&&physical_brightness==255);
 brightness_fail_at=brightness_calls+1;
 assert(local_display_active(false)&&physical_active&&s_state.active&&physical_brightness==255);
 assert(!local_display_active(false)&&!physical_active&&!physical_brightness);
 /* The owner, not the timers, has stopped this display. */
 assert(local_display_state(&snapshot)&&!snapshot.dark);
 brightness_fail_at=brightness_calls+1;
 assert(local_display_active(true)&&!physical_active&&!s_state.active&&!physical_brightness);
 local_display_wake();local_display_tick();assert(!physical_brightness);
 /* A new boot loads the committed settings, not failed candidates. */
 s_mutex=NULL;memset(&s_state,0,sizeof(s_state));now=0;
 assert(!local_display_settings_init(270,&driver,true));assert(!local_display_prepare()&&!local_display_ready());
 uint16_t rotation;assert(registered.get_rotation(&rotation,NULL)&&rotation==90);
 assert(registered.get_backlight(&bl,NULL)&&bl.max_brightness==255&&!bl.dim_after&&!bl.off_after);
 /* Reset boot ignores the saved settings; later start reuses the defaults. */
 s_mutex=NULL;memset(&s_state,0,sizeof(s_state));
 uint32_t previous_saved[5];memcpy(previous_saved,saved,sizeof(saved));
 assert(!local_display_settings_init(270,&driver,false));
 assert(!local_display_prepare()&&!local_display_ready()&&!local_display_active(true));
 assert(registered.get_rotation(&rotation,NULL)&&rotation==270);
 assert(registered.get_backlight(&bl,NULL)&&bl.max_brightness==35&&!bl.dim_after&&!bl.off_after);
 now=INT64_MAX;local_display_tick();assert(physical_brightness==35);
 assert(!memcmp(previous_saved,saved,sizeof(saved)));
 assert(!local_display_settings_init(270,&driver,true));
 assert(registered.get_rotation(&rotation,NULL)&&rotation==270);
 puts("Display settings: dim/off/wake and dark state, never, inactive display, persistence and hardware/store rollback: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(prefix+source+main)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
