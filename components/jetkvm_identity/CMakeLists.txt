idf_component_register(
    SRCS "jetkvm_identity_format.c" "jetkvm_identity_esp.c"
    INCLUDE_DIRS "include"
    REQUIRES efuse esp_hw_support
)
