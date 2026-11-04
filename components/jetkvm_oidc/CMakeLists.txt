idf_component_register(
    SRCS
        "jetkvm_oidc_core.c"
        "jetkvm_oidc_mbedtls.c"
        "jetkvm_oidc_esp.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_http_client
        esp_timer
        mbedtls
        espressif__cjson
        jetkvm_memory_policy
)

# Bench builds only (see JETKVM_MOCK_CLOUD_JWKS_URL in the top-level
# CMakeLists.txt): fetch the keys from the e2e mock cloud.
if(JETKVM_MOCK_CLOUD_JWKS_URL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        JETKVM_MOCK_CLOUD_JWKS_URL="${JETKVM_MOCK_CLOUD_JWKS_URL}")
endif()
