idf_component_register(SRCS "jetkvm_factory_reset.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_system esp_partition nvs_flash freertos heap)
