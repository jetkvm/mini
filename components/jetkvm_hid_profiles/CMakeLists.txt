idf_component_register(
    SRCS "jetkvm_hid_profiles_core.c"
         "jetkvm_hid_profiles_nvs.c"
         "jetkvm_hid_profiles_runtime.c"
         "jetkvm_jiggler.c"
    INCLUDE_DIRS "include"
    REQUIRES control_protocol esp_hw_support esp_timer freertos heap
             jetkvm_hid_input nvs_flash
)
