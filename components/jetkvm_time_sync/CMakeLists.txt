idf_component_register(
    SRCS "jetkvm_time_sync.c" "jetkvm_time_sync_servers.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip
)
