idf_component_register(
    SRCS "jetkvm_usb_service.c"
    INCLUDE_DIRS "include"
    REQUIRES jetkvm_hid_input jetkvm_usb_config jetkvm_usb_hid
)
