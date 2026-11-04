#!/usr/bin/env python3
"""Provisioning scan results must reach a client that streams them by index.

The setup page reads results during the scan and only asks for indexes above
its cursor; upstream re-sorts by RSSI after every channel, so a late strong
network can land below the cursor. The patch appends in discovery order and
walks both bands one channel per scan.
"""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
MANAGER = Path("managed_components/espressif__network_provisioning/src/manager.c")
PATCH = ROOT / "patches/network_provisioning-1.2.4-scan-one-channel-per-band-slot.patch"

STORE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_SCAN_RESULTS 16
#define SCAN_SLOT_COUNT 42
#define ESP_LOGD(...) (void)0
#define ESP_LOGW(...) (void)0
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
#include <string.h>
typedef struct { uint8_t ssid[33]; uint8_t bssid[6]; int8_t rssi; int authmode; } wifi_ap_record_t;
static struct {
    uint8_t channels_per_group;
    uint16_t ap_list_len[SCAN_SLOT_COUNT];
    wifi_ap_record_t *ap_list[SCAN_SLOT_COUNT];
    wifi_ap_record_t *ap_list_sorted[MAX_SCAN_RESULTS];
} ctx, *prov_ctx = &ctx;
static const char *TAG = "t";

HELPER
/* One scan slot delivered `count` records with the given RSSIs; names are
 * optional (NULL keeps every record on a distinct network). */
static void store_named(uint16_t curr_channel, const int8_t *rssi, const char *const *names, uint16_t get_count) {
    esp_err_t ret = ESP_FAIL;
    uint16_t used = 0;
    (void)used;
    USED
    prov_ctx->ap_list[curr_channel] = calloc(get_count, sizeof(wifi_ap_record_t));
    for (uint16_t i = 0; i < get_count; i++) {
        prov_ctx->ap_list[curr_channel][i].rssi = rssi[i];
        snprintf((char *)prov_ctx->ap_list[curr_channel][i].ssid, 33, "%s", names ? names[i] : "");
        if (!names) snprintf((char *)prov_ctx->ap_list[curr_channel][i].ssid, 33, "n%u-%u", curr_channel, i);
    }
    prov_ctx->ap_list_len[curr_channel] = get_count;
STORE_BLOCK
    (void)ret;
}
static void store(uint16_t curr_channel, const int8_t *rssi, uint16_t get_count) { store_named(curr_channel, rssi, NULL, get_count); }

static uint16_t count(void) {
    uint16_t n = 0;
    while (n < MAX_SCAN_RESULTS && prov_ctx->ap_list_sorted[n]) n++;
    return n;
}

int main(void) {
    /* A streaming client: fetch every index once, never look back. */
    int seen_strong = 0;
    uint16_t cursor = 0;
    static const int8_t first[7] = {-70, -71, -72, -74, -78, -80, -82};
    static const int8_t late[1] = {-50};
    store(1, first, 7);
    for (; cursor < count(); cursor++) seen_strong |= prov_ctx->ap_list_sorted[cursor]->rssi == -50;
    store(6, late, 1);
    for (; cursor < count(); cursor++) seen_strong |= prov_ctx->ap_list_sorted[cursor]->rssi == -50;
    assert(count() == 8);
    /* Patched only: a second BSSID of a known SSID upgrades the entry in place. */
    if (seen_strong) {
        static const int8_t dup[2] = {-80, -40};
        static const char *const dup_names[2] = {"office", "office"};
        store_named(36, dup, dup_names, 2);
        assert(count() == 9 && prov_ctx->ap_list_sorted[8]->rssi == -40);
        static const int8_t weaker[1] = {-90};
        static const char *const weaker_names[1] = {"office"};
        store_named(40, weaker, weaker_names, 1);
        assert(count() == 9 && prov_ctx->ap_list_sorted[8]->rssi == -40);
    }
    return seen_strong ? 0 : 1;
}
'''

WALK_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
DEFINES
static struct { uint8_t channels_per_group; struct { uint8_t channel; } scan_cfg; } ctx, *prov_ctx = &ctx;
WALK_FUNCS
static int dfs(uint8_t channel) { return channel >= 52 && channel <= 144; }
int main(void) {
    /* A full request visits 13 + 28 channels once each, selected by number:
     * 5 GHz before 2.4 GHz (the setup page has already walked 2.4 GHz in a
     * grouped pass), and the active non-DFS 5 GHz channels before the
     * passive DFS ones. */
    prov_ctx->channels_per_group = 0;
    unsigned steps = 0, five_ghz = 0, visits[256] = {0};
    int seen_2g = 0, seen_dfs = 0;
    for (uint16_t slot = scan_slot_first(); slot; slot = scan_slot_next(slot)) {
        scan_slot_configure(slot);
        uint8_t channel = prov_ctx->scan_cfg.channel;
        assert(channel != 0 && ++visits[channel] == 1);
        if (channel > 14) {
            assert(!seen_2g);
            assert(dfs(channel) || !seen_dfs);
            seen_dfs |= dfs(channel);
            five_ghz++;
        } else {
            seen_2g = 1;
        }
        steps++;
    }
    assert(steps == 41 && five_ghz == 28);
    /* A grouped request stays on 2.4 GHz and walks all 13 channels. */
    prov_ctx->channels_per_group = 3;
    steps = 0;
    for (uint16_t slot = scan_slot_first(); slot; slot = scan_slot_next(slot)) {
        scan_slot_configure(slot);
        assert(prov_ctx->scan_cfg.channel >= 1 && prov_ctx->scan_cfg.channel <= 13);
        steps++;
    }
    assert(steps == 13);
    return 0;
}
'''


FETCH_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_SCAN_RESULTS 16
#define SCAN_SLOT_COUNT 42
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ESP_LOGD(...) (void)0
#define ESP_LOGE(...) (void)0
#define ESP_LOGW(...) (void)0
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
typedef struct { uint8_t ssid[33]; uint8_t bssid[6]; int8_t rssi; int authmode; } wifi_ap_record_t;
static struct {
    uint8_t channels_per_group;
    uint16_t ap_list_len[SCAN_SLOT_COUNT];
    wifi_ap_record_t *ap_list[SCAN_SLOT_COUNT];
    wifi_ap_record_t *ap_list_sorted[MAX_SCAN_RESULTS];
} ctx, *prov_ctx = &ctx;
static const char *TAG = "t";
/* The driver's records for the slot that just finished scanning. */
static wifi_ap_record_t driver[2] = {{.ssid = "known", .rssi = -60}, {.ssid = "fresh", .rssi = -70}};
static esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *out) {
    *n = MIN(*n, 2);
    memcpy(out, driver, *n * sizeof(*out));
    return ESP_OK;
}
static void esp_wifi_clear_ap_list(void) {}
HELPER
static void fetch(uint16_t curr_channel, uint16_t count) {
    esp_err_t ret = ESP_FAIL;
FETCH_BLOCK
    ret = ESP_OK;
exit:
    (void)ret;
}
int main(void) {
    /* One entry is left, and the slot reports a known SSID before a new one:
     * the known record takes no entry, so the new network must be listed. */
    static wifi_ap_record_t listed[MAX_SCAN_RESULTS - 1];
    for (int i = 0; i < MAX_SCAN_RESULTS - 1; i++) {
        snprintf((char *)listed[i].ssid, 33, i ? "n%d" : "known", i);
        prov_ctx->ap_list_sorted[i] = &listed[i];
    }
    fetch(20, 2);
    assert(prov_ctx->ap_list_sorted[MAX_SCAN_RESULTS - 1] != NULL);
    assert(strcmp((char *)prov_ctx->ap_list_sorted[MAX_SCAN_RESULTS - 1]->ssid, "fresh") == 0);
    return 0;
}
'''


def cc(work, name, text):
    src = work / f"{name}.c"
    src.write_text(text)
    binary = work / name
    subprocess.run(["cc", "-std=c11", str(src), "-o", str(binary)], check=True)
    return subprocess.run([str(binary)]).returncode


with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    target = work / MANAGER
    target.parent.mkdir(parents=True)
    target.write_text((ROOT / MANAGER).read_text())
    reverse = subprocess.run(["git", "apply", "--reverse", "--check", str(PATCH)],
                             cwd=work, capture_output=True)
    if reverse.returncode == 0:
        subprocess.run(["git", "apply", "--reverse", str(PATCH)], cwd=work, check=True)
    for patched in (False, True):
        if patched:
            subprocess.run(["git", "apply", str(PATCH)], cwd=work, check=True)
        source = target.read_text()
        start = re.search(r"    /\* (Store results in sorted list|Append in discovery order)", source).start()
        end = source.index("    ret = ESP_OK;\nexit:", start)
        helper = ""
        if "static uint16_t scan_list_used(void)" in source:
            h0 = source.index("/* Entries appended so far")
            helper = source[h0:source.index("static esp_err_t update_wifi_scan_results(void)")]
        harness = (STORE_HARNESS.replace("STORE_BLOCK", source[start:end])
                   .replace("HELPER", helper)
                   .replace("USED", "used = scan_list_used();" if helper else ""))
        rc = cc(work, f"store{patched}", harness)
        assert rc == (0 if patched else 1), (patched, rc)
        if patched:
            defines = source[source.index("#define SCAN_SLOT_2G_COUNT"):source.index("#define SCAN_WALK_GROUP")]
            funcs = source[source.index("static const uint8_t scan_5g_channels"):source.index("/* Entries appended so far")]
            harness = WALK_HARNESS.replace("DEFINES", defines).replace("WALK_FUNCS", funcs)
            assert cc(work, "walk", harness) == 0
            f0 = source.index("    /* Fetch up to a full list")
            harness = (FETCH_HARNESS.replace("FETCH_BLOCK", source[f0:end])
                       .replace("HELPER", helper))
            assert cc(work, "fetch", harness) == 0
    subprocess.run(["git", "apply", "--reverse", "--check", str(PATCH)], cwd=work, check=True)
print("Provisioning scan walk regression passed (original hides late strong AP, patch delivers it)")
