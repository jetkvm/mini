idf_component_register(
    SRCS "jetkvm_settings_core.c"
         "jetkvm_settings_nvs.c"
         "jetkvm_settings_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos nvs_flash
)
