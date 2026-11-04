idf_component_register(
    SRCS "jetkvm_usb_hid.c" "keyboard_rollover.c" "cdrom_toc.c"
    INCLUDE_DIRS "include"
    REQUIRES jetkvm_virtual_media esp_timer jetkvm_audio)

target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
