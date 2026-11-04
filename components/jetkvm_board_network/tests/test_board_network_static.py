#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "jetkvm_board_network.c").read_text()
header = (root / "include/jetkvm_board_network.h").read_text()
c5_source = (root.parent / "jetkvm_c5_wifi/jetkvm_c5_wifi.c").read_text()
main = (root.parents[1] / "main/main.c").read_text()
for owner in ("eth_event_handler", "screen_status_task", "ESP_NETIF_DEFAULT_ETH"):
    assert owner in source
    assert owner not in main
assert "ESP_NETIF_DHCP_CLIENT" in source
assert "esp_netif_get_flags(eth_netif)" in source
assert "ETH_STATIC_IP_ADDR" not in source
assert "esp_netif_dhcpc_stop" not in source
# The EMAC takes ESP_MAC_ETH at driver install, so the MAC setting comes first.
assert source.index("jetkvm_mac_address_apply_ethernet()") < source.index("esp_eth_driver_install(")
assert source.index("jetkvm_network_settings_init(eth_netif)") < source.index("esp_eth_start(eth_handle)")
c5_startup = c5_source[c5_source.index("static esp_err_t c5_wifi_start_impl("):c5_source.index("static void c5_worker_task(")]
assert c5_startup.index("jetkvm_network_settings_init(s_station)") < c5_startup.index("esp_wifi_start()")
for api in (
    "jetkvm_board_network_screen_start",
    "jetkvm_board_network_ethernet_start",
    "jetkvm_board_network_screen_status_start",
):
    assert api in source
    assert api in main
assert main.index("jetkvm_board_network_screen_start") < main.index("jetkvm_board_network_ethernet_start")
assert main.index("jetkvm_board_network_ethernet_start") < main.index("webrtc_transport_start")
assert main.index("webrtc_transport_start") < main.index("jetkvm_board_network_screen_status_start")
wifi_state_api = "jetkvm_board_network_set_wifi_state"
assert wifi_state_api in source
assert wifi_state_api in header
assert wifi_state_api in c5_source
assert "IP_EVENT_STA_GOT_IP" in c5_source
assert "IP_EVENT_STA_LOST_IP" in c5_source
assert "jetkvm_board_network_set_wifi_details" in source
assert "jetkvm_board_network_set_wifi_details" in header
assert "esp_wifi_sta_get_ap_info" in c5_source
assert "C5_STATUS_UPDATE_MS" in c5_source
print("board network and screen status boundary: PASS")
