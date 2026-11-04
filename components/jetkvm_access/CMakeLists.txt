idf_component_register(SRCS "password_core.c" "password_pbkdf2.c" "jetkvm_access.c" "jetkvm_tls.c"
    INCLUDE_DIRS "include"
    REQUIRES control_protocol esp_netif esp_http_server nvs_flash mbedtls esp_timer espressif__cjson
    PRIV_REQUIRES jetkvm_factory_reset)
