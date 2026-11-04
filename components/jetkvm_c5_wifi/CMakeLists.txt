set(_jetkvm_c5_embed_files)
if(JETKVM_BOARD_VARIANT STREQUAL "wifi")
    list(APPEND _jetkvm_c5_embed_files
        "firmware/c5_bootloader_zlib.bin"
        "firmware/c5_partition_table_zlib.bin"
        "firmware/c5_ota_data_initial_zlib.bin"
        "firmware/c5_network_adapter_zlib.bin")
endif()

if(JETKVM_BOARD_VARIANT STREQUAL "wifi")
    idf_component_register(
        SRCS "jetkvm_c5_wifi.c" "wifi_network_list.c" "wifi_join_failure.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_hosted esp_wifi esp_wifi_remote jetkvm_board_config
            jetkvm_board_network jetkvm_network_settings nvs_flash
        PRIV_REQUIRES esp_app_format esp_driver_gpio esp_event esp_hw_support esp_netif heap
            espressif__cjson espressif__network_provisioning bt jetkvm_factory_reset protocomm
        EMBED_FILES ${_jetkvm_c5_embed_files}
    )

    # esp_wifi_remote replaces esp_wifi.h on Wi-Fi-less hosts. Its injected
    # include directory is added to esp_wifi after component dependency
    # propagation, so consumers with a direct esp_wifi dependency do not
    # reliably inherit it on IDF 6.0. Add the official generated directory to
    # this component explicitly.
    idf_component_get_property(_jetkvm_wifi_remote_dir
        espressif__esp_wifi_remote COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${_jetkvm_wifi_remote_dir}/idf_v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}/include/injected")

    # Bench builds only (see JETKVM_WIFI_SETUP_LOG_CODE in the top-level
    # CMakeLists.txt): a fixed BLE setup code, written to the log.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        JETKVM_WIFI_SETUP_LOG_CODE=${JETKVM_WIFI_SETUP_LOG_CODE})
else()
    idf_component_register(
        SRCS "jetkvm_c5_wifi.c" "wifi_network_list.c" "wifi_join_failure.c"
        INCLUDE_DIRS "include"
        # The initial minimal-build dependency scan does not receive the
        # project-specific board-variant cache value. Keep common IDF
        # dependencies visible in this branch as well.
        REQUIRES jetkvm_board_config jetkvm_board_network jetkvm_network_settings nvs_flash
    )
endif()
