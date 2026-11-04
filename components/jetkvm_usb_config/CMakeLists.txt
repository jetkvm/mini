idf_component_register(
    SRCS "jetkvm_usb_config_core.c" "jetkvm_usb_config_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
