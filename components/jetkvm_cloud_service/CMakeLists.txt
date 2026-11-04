idf_component_register(
    SRCS "jetkvm_cloud_service.c" "jetkvm_cloud_service_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES jetkvm_cloud_client jetkvm_memory_policy jetkvm_oidc
    PRIV_REQUIRES freertos jetkvm_board_config jetkvm_device_state
                  jetkvm_time_sync log
)
