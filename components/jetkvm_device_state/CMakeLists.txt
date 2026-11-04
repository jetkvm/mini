idf_component_register(
    SRCS "jetkvm_device_state.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos jetkvm_cloud_config jetkvm_identity
             jetkvm_memory_policy
)
