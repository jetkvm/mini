idf_component_register(
    SRCS "jetkvm_usb_debugging.c"
         "jetkvm_usb_debugging_setting.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES control_protocol esp_driver_gpio esp_hal_usb esp_hw_support log nvs_flash soc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
