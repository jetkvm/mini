#!/usr/bin/env python3
from pathlib import Path


component = Path(__file__).resolve().parents[1]
repo = component.parents[1]
source = (component / "jetkvm_c5_wifi.c").read_text()
root_cmake = (repo / "CMakeLists.txt").read_text()
patch = (repo / "patches/network_provisioning-1.2.4-configurable-wifi-storage.patch").read_text()

assert '#define C5_PROV_NVS_NETWORKS_KEY "networks"' in source
assert '#define C5_PROV_NVS_C5_CLEARED_KEY "c5_clean"' in source
cred_success = source[source.index("if (event == NETWORK_PROV_WIFI_CRED_SUCCESS)"):]
assert "c5_store_network(" in cred_success[:cred_success.index("\n}\n")]
assert ".wifi_storage = WIFI_STORAGE_RAM" in source
assert "esp_wifi_set_storage(WIFI_STORAGE_RAM)" in source
assert "esp_wifi_set_storage(WIFI_STORAGE_FLASH)" not in source
assert "c5_clear_legacy_credentials_once()" in source
assert "#define C5_STATUS_TASK_STACK_BYTES (16U * 1024U)" in source
got_ip_block = source[
    source.index("event_id == IP_EVENT_STA_GOT_IP") :
    source.index("event_id == IP_EVENT_STA_LOST_IP")
]
assert "c5_start_status_task_if_needed();" in got_ip_block
ready_block = source[source.index("s_c5_wifi_ready = true;") :]
assert 'c5_status_task, "c5_wifi_status"' not in ready_block

patch_name = "network_provisioning-1.2.4-configurable-wifi-storage.patch"
assert f"apply_managed_component_patch(patches/{patch_name})" in root_cmake
assert "+    wifi_storage_t wifi_storage;" in patch
assert "esp_wifi_set_storage(prov_ctx->mgr_config.wifi_storage)" in patch

print("ESP32-C5 credentials are P4-owned and C5-RAM-only")

# Association alone must not complete a frequency change before DHCP succeeds.
wifi_events = source[source.index("if (event_base == WIFI_EVENT) {"):source.index("if (event_base == IP_EVENT) {")]
associated = wifi_events[wifi_events.index("event_id == WIFI_EVENT_STA_CONNECTED"):
                         wifi_events.index("event_id == WIFI_EVENT_STA_STOP")]
assert "s_station_connected" not in associated
assert "__atomic_store_n(&s_station_associated, true" in associated
assert wifi_events.count("event_id == WIFI_EVENT_STA_STOP") == 1
assert "__atomic_store_n(&s_station_connected, true" in got_ip_block
lost_ip_block = source[source.index("event_id == IP_EVENT_STA_LOST_IP"):source.index("static void c5_refresh_link_details")]
assert "__atomic_store_n(&s_station_connected, false" in lost_ip_block
