idf_component_register(
    SRCS "jetkvm_os_services.c" "os_services_usb.c" "os_services_identity.c"
         "os_services_session.c" "os_services_video_rate.c"
         "os_services_wire.c" "os_services_link.c"
         "os_services_settings.c" "os_services_channels.c"
         "os_services_edid.c" "os_services_pgp.c" "os_services_update.c"
         "os_services_release.c" "os_services_relay.c"
         "third_party/monocypher/monocypher.c"
         "third_party/monocypher/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "third_party/monocypher"
    REQUIRES espressif__cjson esp_common
    PRIV_REQUIRES jetkvm_usb_hid jetkvm_memory_policy jetkvm_ota jetkvm_timing_log jetkvm_video
                  jetkvm_video_quality webrtc_transport esp_timer esp_hw_support esp_http_client
                  esp_mm freertos heap mbedtls nvs_flash log)

# The service task's stack is 8 KiB, so no function may keep a large buffer
# on it. Monocypher's Argon2 exceeds this but is unused; its signature check
# runs only on the update worker's 10 KiB stack (os_services_relay.c).
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror -Wframe-larger-than=1024)
set_source_files_properties(third_party/monocypher/monocypher.c
                            PROPERTIES COMPILE_OPTIONS -Wno-frame-larger-than)

# Bench builds only (see JETKVM_MOCK_CLOUD_JWKS_URL in the top-level
# CMakeLists.txt): the release bucket is the e2e mock's.
if(JETKVM_MOCK_CLOUD_JWKS_URL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE JETKVM_MOCK_CLOUD=1)
endif()
