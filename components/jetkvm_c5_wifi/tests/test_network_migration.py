#!/usr/bin/env python3
"""Run the production saved-network loader against an in-memory NVS."""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / 'jetkvm_c5_wifi.c').read_text()


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wifi_network_list.h"
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define NVS_READWRITE 1
#define portMAX_DELAY 0
#define C5_PROV_NVS_NAMESPACE "wifi_prov"
#define C5_PROV_NVS_NETWORKS_KEY "networks"
#define C5_PROV_NVS_STA_KEY "sta_cfg"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef struct { uint8_t ssid[32]; uint8_t password[64]; uint8_t bssid[6]; uint32_t flags[8]; } wifi_sta_config_t;
static void *s_networks_lock;
static char bound[40];
static int binds, fail_bind;
static int jetkvm_network_settings_bind_static_if_unbound(const char *ssid) {binds++;snprintf(bound,sizeof(bound),"%s",ssid);return fail_bind;}
static wifi_network_list_t s_networks;
static int xSemaphoreTake(void *l, int t) {(void)l;(void)t;return 1;}
static void xSemaphoreGive(void *l) {(void)l;}

/* Two keys, each an optional blob. */
static struct { const char *key; uint8_t data[1024]; size_t length; bool present; } store[2] = {
    {.key = "networks"}, {.key = "sta_cfg"},
};
static int commits, fail_set;
static int slot(const char *key) {for (int i = 0; i < 2; ++i) if (!strcmp(store[i].key, key)) return i; assert(0); return -1;}
static int nvs_open(const char *n, int mode, int *h) {(void)n;(void)mode;*h=1;return 0;}
static void nvs_close(int h) {(void)h;}
static int nvs_commit(int h) {(void)h;commits++;return 0;}
static int nvs_get_blob(int h, const char *key, void *out, size_t *length)
{
    (void)h;
    int i = slot(key);
    if (!store[i].present) return ESP_ERR_NVS_NOT_FOUND;
    if (out == NULL) {*length = store[i].length; return ESP_OK;}
    if (*length < store[i].length) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, store[i].data, store[i].length);
    *length = store[i].length;
    return ESP_OK;
}
static int nvs_set_blob(int h, const char *key, const void *data, size_t length)
{
    (void)h;
    if (fail_set) return 7;
    int i = slot(key);
    memcpy(store[i].data, data, length);
    store[i].length = length;
    store[i].present = true;
    return ESP_OK;
}
static int nvs_erase_key(int h, const char *key) {(void)h;store[slot(key)].present = false;return ESP_OK;}
static void put(const char *key, const void *data, size_t length)
{
    int i = slot(key);
    memcpy(store[i].data, data, length);
    store[i].length = length;
    store[i].present = true;
}
static void reset(void) {memset(store[0].data,0,1024);memset(store[1].data,0,1024);store[0].present=store[1].present=false;commits=fail_set=fail_bind=0;memset(&s_networks,0,sizeof(s_networks));}
static wifi_sta_config_t legacy(const char *ssid, const char *password)
{
    wifi_sta_config_t config = {0};
    memcpy(config.ssid, ssid, strlen(ssid));
    memcpy(config.password, password, strlen(password));
    return config;
}
'''

test = r'''
int main(void)
{
    /* Upgrade: the single record becomes entry 0; its key stays as the copy
     * that older firmware reads after a downgrade. */
    reset();
    wifi_sta_config_t old = legacy("Home", "password1");
    put("sta_cfg", &old, sizeof(old));
    assert(c5_load_networks() == ESP_OK);
    assert(s_networks.count == 1 && wifi_network_list_find(&s_networks, (const uint8_t *)"Home", 4) == 0);
    assert(store[1].present && store[0].present);
    /* Static IPv4 settings made for the single network stay with it. */
    assert(binds == 1 && strcmp(bound, "Home") == 0);
    wifi_network_list_t stored;
    assert(wifi_network_list_decode(&stored, store[0].data, store[0].length) && stored.count == 1);
    assert(stored.items[0].password_len == 9 && !memcmp(stored.items[0].password, "password1", 9));

    /* Next boot: the list loads unchanged and nothing is written. */
    memset(&s_networks, 0, sizeof(s_networks));
    commits = 0;
    assert(c5_load_networks() == ESP_OK && s_networks.count == 1 && commits == 0 && binds == 1);

    /* A list already exists: a record for a network not in the list (older
     * firmware wrote it) is erased, not merged. */
    wifi_sta_config_t other = legacy("Stale", "password2");
    put("sta_cfg", &other, sizeof(other));
    assert(c5_load_networks() == ESP_OK && s_networks.count == 1 && !store[1].present);
    assert(wifi_network_list_find(&s_networks, (const uint8_t *)"Stale", 5) == -1);

    /* A record of the wrong size is dropped. */
    reset();
    put("sta_cfg", &old, sizeof(old) - 4);
    assert(c5_load_networks() == ESP_OK && s_networks.count == 0 && !store[1].present);

    /* A damaged list is discarded; the device then starts BLE setup. */
    reset();
    put("networks", "garbage", 7);
    assert(c5_load_networks() == ESP_OK && s_networks.count == 0 && !store[0].present);

    /* If the list cannot be written, the old record stays for the next boot. */
    reset();
    put("sta_cfg", &old, sizeof(old));
    fail_set = 1;
    assert(c5_load_networks() == 7 && store[1].present && s_networks.count == 0);

    /* If the static settings cannot be bound, the legacy record stays for a
     * retry, and the network is still used from RAM. */
    reset();
    put("sta_cfg", &old, sizeof(old));
    fail_bind = 9;
    assert(c5_load_networks() == ESP_OK && store[1].present && !store[0].present && s_networks.count == 1);
    fail_bind = 0;
    assert(c5_load_networks() == ESP_OK && store[1].present && store[0].present);

    puts("C5 saved network load and sta_cfg migration: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(stub + function('static esp_err_t c5_load_networks(void)') + test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-D_DEFAULT_SOURCE', f'-I{component / "include"}',
                    str(path / 'test.c'), str(component / 'wifi_network_list.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
