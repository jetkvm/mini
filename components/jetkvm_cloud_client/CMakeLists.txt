idf_component_register(
    SRCS "jetkvm_cloud_core.c"
         "jetkvm_cloud_client.c"
         "jetkvm_cloud_transport_adapter.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES cloud_signaling webrtc_transport espressif__esp_websocket_client
             esp_event esp_netif esp_timer freertos heap jetkvm_memory_policy
             log mbedtls
)
