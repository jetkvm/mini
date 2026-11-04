idf_component_register(
    SRCS "jetkvm_cloud_registration_core.c"
         "jetkvm_cloud_registration_esp.c"
         "jetkvm_cloud_public_ip.c"
    INCLUDE_DIRS "include"
    REQUIRES jetkvm_cloud_config
    PRIV_REQUIRES esp_http_client espressif__cjson mbedtls jetkvm_memory_policy
)

# Bench builds only (see JETKVM_MOCK_CLOUD_JWKS_URL in the top-level
# CMakeLists.txt): allow the e2e mock cloud's http:// URL.
if(JETKVM_MOCK_CLOUD_JWKS_URL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE JETKVM_MOCK_CLOUD=1)
endif()
