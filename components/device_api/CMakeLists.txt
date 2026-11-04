idf_component_register(
    SRCS "device_api.c" "device_contract.c"
    INCLUDE_DIRS "include"
    REQUIRES jetkvm_access esp_http_server espressif__cjson
             jetkvm_cloud_registration jetkvm_device_state jetkvm_oidc
             jetkvm_time_sync jetkvm_memory_policy
    PRIV_REQUIRES jetkvm_board_config jetkvm_factory_reset esp_app_format
                  cloud_signaling
)
