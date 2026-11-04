#!/usr/bin/env python3
"""Exercise C5 credential cleanup and failure ordering using the production function."""
from pathlib import Path
import subprocess
import tempfile
source = (Path(__file__).resolve().parents[1] / 'jetkvm_c5_wifi.c').read_text()
start = source.index('static esp_err_t c5_clear_legacy_credentials_once(')
end = source.index('\nstatic void c5_provisioning_app_event_handler', start)
stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 1
#define C5_PROV_NVS_NAMESPACE "wifi_prov"
#define C5_PROV_NVS_C5_CLEARED_KEY "c5_clean"
#define ESP_LOGI(...) ((void)0)
static int marker, legacy, fail_restore, fail_commit;
static int restores, commits;
static int nvs_open(const char *n, int mode, int *h) {(void)n;(void)mode;*h=1;return 0;}
static void nvs_close(int h) {(void)h;}
static int nvs_get_u8(int h,const char *k,uint8_t *v) {(void)h;(void)k;*v=marker;return marker?0:1;}
static int nvs_set_u8(int h,const char *k,uint8_t v) {(void)h;(void)k;marker=v;return 0;}
static int nvs_commit(int h) {(void)h;commits++;return fail_commit?9:0;}
static int esp_wifi_restore(void) {restores++;if(fail_restore)return 8;legacy=0;return 0;}
static void reset(void) {marker=legacy=fail_restore=fail_commit=restores=commits=0;}
'''
test = r'''
int main(void) {
 reset(); legacy=1;
 assert(c5_clear_legacy_credentials_once()==0);
 assert(!legacy && marker && restores==1 && commits==1);
 assert(c5_clear_legacy_credentials_once()==0 && restores==1 && commits==1);
 reset(); legacy=fail_restore=1;
 assert(c5_clear_legacy_credentials_once()==8 && legacy && !marker && commits==0);
 fail_restore=0;
 assert(c5_clear_legacy_credentials_once()==0 && !legacy && marker && restores==2);
 reset(); fail_commit=1;
 assert(c5_clear_legacy_credentials_once()==9 && commits==1);
 reset(); /* a clean module is marked too */
 assert(c5_clear_legacy_credentials_once()==0 && marker && restores==1);
 puts("C5 credential cleanup: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(stub + source[start:end] + test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
