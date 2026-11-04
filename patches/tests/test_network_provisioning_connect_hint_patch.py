#!/usr/bin/env python3
"""The provisioning join starts its scan on the channel the setup scan found.

Without a channel the fast connect scan starts on channel 1 and, for a 5 GHz
network, walks most of both bands first; the BLE link that reports the join
gets too little airtime meanwhile. The hint must also match a 32-byte SSID,
which fills wifi_sta_config_t.ssid with no terminator (the password follows).
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
MANAGER = ROOT / "managed_components/espressif__network_provisioning/src/manager.c"
PATCH = ROOT / "patches/network_provisioning-1.2.4-connect-channel-hint.patch"

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define MAX_SCAN_RESULTS 8
#define ESP_LOGI(tag, ...) (void)(tag)
typedef struct { uint8_t ssid[33]; uint8_t bssid[6]; uint8_t primary; int8_t rssi; } wifi_ap_record_t;
typedef struct { uint8_t ssid[32]; uint8_t password[64]; bool bssid_set; uint8_t bssid[6]; uint8_t channel; } wifi_sta_config_t;
static struct { wifi_ap_record_t *ap_list_sorted[MAX_SCAN_RESULTS]; } ctx, *prov_ctx = &ctx;
static const char *TAG = "t";
FUNCS
static wifi_sta_config_t sta_for(const char *ssid) {
    wifi_sta_config_t sta;
    memset(&sta, 0, sizeof(sta));
    memcpy(sta.ssid, ssid, strlen(ssid));
    strcpy((char *)sta.password, "modernunicorn139");
    return sta;
}
int main(void) {
    static wifi_ap_record_t office = {.ssid = "office", .primary = 6};
    static wifi_ap_record_t longname = {.ssid = "0123456789abcdef0123456789abcdef", .primary = 149};
    prov_ctx->ap_list_sorted[0] = &office;
    prov_ctx->ap_list_sorted[1] = &longname;

    wifi_sta_config_t sta = sta_for("office");
    wifi_connect_channel_hint(&sta);
    assert(sta.channel == 6);

    sta = sta_for("0123456789abcdef0123456789abcdef");
    wifi_connect_channel_hint(&sta);
    assert(sta.channel == 149);

    sta = sta_for("elsewhere");
    wifi_connect_channel_hint(&sta);
    assert(sta.channel == 0);

    sta = sta_for("office");
    sta.channel = 11;
    wifi_connect_channel_hint(&sta);
    assert(sta.channel == 11);
    return 0;
}
'''


source = MANAGER.read_text()
subprocess.run(["git", "apply", "--reverse", "--check", str(PATCH)], cwd=ROOT, check=True)
helpers = source[source.index("/* Entries appended so far"):
                 source.index("static esp_err_t update_wifi_scan_results(void)")]
hint = source[source.index("/* Start the connect scan on the channel"):
              source.index("static void wifi_connect_timer_cb")]
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    src = work / "hint.c"
    src.write_text(HARNESS.replace("FUNCS", helpers + hint))
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", str(src), "-o", str(work / "hint")],
                   check=True)
    subprocess.run([str(work / "hint")], check=True)
print("Provisioning connect channel hint passed (32-byte SSID, no match, preset channel)")
