idf_component_register(
    SRCS "jetkvm_video.c" "jetkvm_video_helpers.c"
    INCLUDE_DIRS "include"
    REQUIRES
        device_api
        dma_cache_fastpath
        jetkvm_board_config
        jetkvm_hdmi_bridge
        jetkvm_memory_policy
        jetkvm_screen
        jetkvm_settings
        jetkvm_video_quality
        p4_csi_capture
        webrtc_transport
        esp_driver_cam
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_isp
        esp_hw_support
        esp_psram
        esp_system
        esp_timer
        espressif__esp_h264
    PRIV_REQUIRES soc esp_rom
    LDFRAGMENTS "linker.lf"
)

# The capture orchestrator still sequences a small set of bridge registers.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_SOURCE_DIR}/components/jetkvm_hdmi_bridge")
