#!/usr/bin/env python3
"""The C5 takes the factory MAC before the radio and the BLE controller start."""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "jetkvm_c5_wifi.c").read_text()
start = source[source.index("static esp_err_t c5_wifi_start_impl("):]
assert start.index("esp_wifi_set_mode(WIFI_MODE_STA)") < start.index("c5_apply_factory_mac();")
assert start.index("c5_apply_factory_mac();") < start.index("error = esp_wifi_start();")
# Every later station start (band switch, radio restart) sets the MAC again.
assert source.count("esp_wifi_start()") == 2, "only c5_station_start and the first start call esp_wifi_start"
helper = source[source.index("static esp_err_t c5_station_start("):source.index("static esp_err_t c5_apply_band(")]
assert helper.index("esp_wifi_set_mac(WIFI_IF_STA, s_factory_mac)") < helper.index("esp_wifi_start()")
ble = source[source.index("static esp_err_t c5_start_ble_provisioning("):]
assert ble.index("esp_hosted_iface_mac_addr_set(") < ble.index("esp_hosted_bt_controller_init()")
# The setup name and the setup page read the station MAC, so both follow it.
assert '"JetKVM_%02X%02X%02X"' in source
print("C5 factory MAC before esp_wifi_start and the BLE controller init: PASS")
