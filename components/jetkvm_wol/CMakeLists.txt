idf_component_register(
    SRCS "jetkvm_wol.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos nvs_flash
)
