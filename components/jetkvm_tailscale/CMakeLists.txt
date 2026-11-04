if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT CONFIG_LWIP_ESP_NETIF_CLIENT_DATA)
    message(FATAL_ERROR
        "Native Tailscale requires CONFIG_LWIP_ESP_NETIF_CLIENT_DATA=y. Enable it "
        "in sdkconfig, then run idf.py reconfigure.")
endif()

idf_component_register(
    SRCS "tailscale.c" "tailscale_config.c"
    INCLUDE_DIRS "include"
    REQUIRES control_protocol esp_common
    PRIV_REQUIRES microlink esp_netif esp_timer freertos nvs_flash
                  jetkvm_network_settings
)
