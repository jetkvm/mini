idf_component_register(
    SRCS "jetkvm_memory_policy.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES espressif__cjson freertos heap log esp_timer
)
