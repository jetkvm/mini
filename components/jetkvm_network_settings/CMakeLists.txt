idf_component_register(
    SRCS "network_settings_core.c" "network_settings.c" "mac_address.c"
    INCLUDE_DIRS "include"
    REQUIRES control_protocol esp_hw_support esp_netif esp_timer freertos jetkvm_identity jetkvm_time_sync lwip nvs_flash
)
