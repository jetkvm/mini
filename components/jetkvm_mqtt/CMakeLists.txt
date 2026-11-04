idf_component_register(
    SRCS "jetkvm_mqtt.c" "jetkvm_mqtt_config.c" "jetkvm_mqtt_store.c"
         "jetkvm_mqtt_rpc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES espressif__mqtt espressif__cjson esp_app_format mbedtls
             esp_event esp_netif esp_system freertos heap nvs_flash
             control_protocol jetkvm_cloud_service jetkvm_device_state
             jetkvm_hid_profiles jetkvm_memory_policy jetkvm_network_settings jetkvm_usb_hid
             jetkvm_virtual_media webrtc_transport
)
