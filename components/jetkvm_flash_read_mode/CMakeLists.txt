idf_component_register(
    SRCS "jetkvm_flash_read_mode.c" "flash_read_switch.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system log soc spi_flash
    LDFRAGMENTS "linker.lf"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
