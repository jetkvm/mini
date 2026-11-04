#!/usr/bin/env python3
"""Exercise production collection with provider and allocation failures."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'components/webrtc_transport/diagnostics.c').read_text()
collect = source[source.index('static cJSON *collect(void)'):source.index('static bool send_chunk')]
# Stub only the external provider structures; compile the actual collector.
video_fields = sorted(set(re.findall(r'video->(\w+)', collect)))
usb_fields = sorted(set(re.findall(r'usb\.(\w+)', collect)))
network_fields = sorted(set(re.findall(r'network\.(\w+)', collect)))
network_bools = {'up', 'online', 'dhcp'}
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#define ESP_OK 0
#define JETKVM_DEVICE_ID_CAPACITY 32
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
static unsigned allocations, failure, live;
static int provider_error;
static void *allocate(size_t size) {
    if (++allocations == failure) return NULL;
    void *p = malloc(size); if (p) ++live; return p;
}
static void release(void *p) { if (p) { assert(live); --live; free(p); } }
#define jetkvm_memory_policy_control_alloc allocate
#define jetkvm_memory_policy_control_free release
'''
prefix += 'typedef struct { double ' + ','.join(video_fields) + '; } webrtc_transport_stats_t;\n'
prefix += 'typedef struct { double ' + ','.join(usb_fields) + '; } jetkvm_usb_service_snapshot_t;\n'
prefix += 'typedef struct {' + ''.join(('int '+f+';' if f in network_bools else 'char '+f+'[64];') for f in network_fields) + '} jetkvm_network_state_t;\n'
prefix += r'''
typedef struct { char project_name[32], version[32], idf_ver[32], date[32], time[32]; unsigned char app_elf_sha256[32]; } esp_app_desc_t;
static int webrtc_transport_get_stats_detailed(webrtc_transport_stats_t *s) { memset(s, 0, sizeof(*s)); return provider_error == 1; }
static int jetkvm_usb_service_get_snapshot(jetkvm_usb_service_snapshot_t *s) { memset(s, 0, sizeof(*s)); return provider_error == 2; }
static int jetkvm_device_state_get_id(char *s, size_t n) { snprintf(s, n, "test-device"); return provider_error == 3; }
static void jetkvm_network_snapshot(jetkvm_network_state_t *s) { memset(s, 0, sizeof(*s)); }
static const esp_app_desc_t *esp_app_get_description(void) { static const esp_app_desc_t a = {.project_name="jetkvm-mini"}; return &a; }
static int64_t esp_timer_get_time(void) { return 5000000; }
static int esp_reset_reason(void) { return 1; }
static size_t heap_caps_get_free_size(int caps) { (void)caps; return 1000; }
static size_t heap_caps_get_minimum_free_size(int caps) { (void)caps; return 500; }
'''
main = r'''
int main(void) {
    cJSON_Hooks hooks = {allocate, release}; cJSON_InitHooks(&hooks);
    cJSON *result = collect(); assert(result);
    assert(cJSON_GetObjectItemCaseSensitive(result, "firmware"));
    unsigned count = allocations; cJSON_Delete(result); assert(!live);
    for (failure = 1; failure <= count; ++failure) {
        allocations = 0; result = collect(); assert(!result); assert(!live);
    }
    failure = 0;
    for (provider_error = 1; provider_error <= 3; ++provider_error) {
        result = collect(); assert(!result); assert(!live);
    }
    puts("Collection provider failures and every allocation failure: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + collect + main)
    cjson = root / 'managed_components/espressif__cjson/cJSON'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I'+str(cjson), str(path/'test.c'), str(cjson/'cJSON.c'), '-lm', '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
