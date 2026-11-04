idf_component_register(
    SRCS "jetkvm_console.c"
    INCLUDE_DIRS "include"
    REQUIRES log esp_ringbuf esp_rom esp_timer freertos heap
)
