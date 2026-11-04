set(_jetkvm_main_priv_requires
        jetkvm_access jetkvm_audio jetkvm_console webrtc_transport control_protocol device_api jetkvm_board_config
        jetkvm_board_network jetkvm_c5_wifi jetkvm_cloud_service
        jetkvm_factory_reset jetkvm_flash_read_mode jetkvm_device_state jetkvm_hid_input
        jetkvm_hid_profiles jetkvm_hdmi_bridge jetkvm_memory_policy
        jetkvm_mqtt jetkvm_network_settings jetkvm_oidc jetkvm_os_services jetkvm_ota jetkvm_screen jetkvm_settings
        jetkvm_time_sync jetkvm_tailscale jetkvm_usb_debugging jetkvm_usb_hid jetkvm_usb_service jetkvm_video
        jetkvm_video_quality jetkvm_virtual_media jetkvm_wol
        esp_driver_gpio esp_system
        heap nvs_flash)

idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES ${_jetkvm_main_priv_requires}
    INCLUDE_DIRS ""
)
# Every restart, a panic's included, resets the whole digital core (main.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_restart_noos")

set(USB_MSC_BENCHMARK_AUTOMOUNT_FIRST_IMAGE OFF CACHE BOOL
    "Mount the largest TF image before TinyUSB starts (benchmark builds only)")
if(USB_MSC_BENCHMARK_AUTOMOUNT_FIRST_IMAGE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        USB_MSC_BENCHMARK_AUTOMOUNT_FIRST_IMAGE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        USB_MSC_BENCHMARK_AUTOMOUNT_FIRST_IMAGE=0)
endif()
