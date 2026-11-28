if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT CONFIG_LWIP_ESP_NETIF_CLIENT_DATA)
    message(FATAL_ERROR
        "Native Tailscale requires CONFIG_LWIP_ESP_NETIF_CLIENT_DATA=y. Enable it "
        "in sdkconfig, then run idf.py reconfigure.")
endif()

idf_component_register(
    SRCS "tailscale.c" "tailscale_config.c" "tunnel_input.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES control_protocol esp_common
    PRIV_REQUIRES microlink esp_netif esp_timer freertos nvs_flash
                  jetkvm_network_settings lwip
)

# The address of the tunnel is taken from the tunnel only (tunnel_input.c).
# GNU ld sends each call of lwIP's ip4_input() to the rule, and -u keeps the
# rule in the link although no source names it.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=ip4_input" "-Wl,-u,__wrap_ip4_input")
