idf_component_register(
    SRCS "jetkvm_hid_input.c"
    INCLUDE_DIRS "include"
    REQUIRES control_protocol jetkvm_usb_hid esp_timer
)
