idf_component_register(
    SRCS "jetkvm_ota.c" "jetkvm_ota_core.c" "jetkvm_ota_rpc.c" "jetkvm_ota_semver.c"
         "jetkvm_ota_signature.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update control_protocol espressif__cjson esp_http_client
             esp_partition esp_system esp_timer freertos heap mbedtls
             nvs_flash jetkvm_board_config jetkvm_device_state
             jetkvm_memory_policy jetkvm_time_sync
    # The OpenPGP check and its hashes (os_services_pgp.h), which the OS
    # Services update relay uses for the sender's packages.
    PRIV_REQUIRES jetkvm_os_services
)

# Bench builds only (see JETKVM_MOCK_CLOUD_JWKS_URL in the top-level
# CMakeLists.txt): an update from a stand-in update server is checked
# against the test release key.
if(JETKVM_MOCK_CLOUD_JWKS_URL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE JETKVM_MOCK_CLOUD=1)
endif()

# The SKU marker is referenced by nothing, so tell the linker to keep it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u jetkvm_image_desc")
