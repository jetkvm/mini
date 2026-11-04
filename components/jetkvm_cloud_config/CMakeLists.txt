idf_component_register(
    SRCS "jetkvm_cloud_config_core.c" "jetkvm_cloud_config_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash jetkvm_memory_policy
)
