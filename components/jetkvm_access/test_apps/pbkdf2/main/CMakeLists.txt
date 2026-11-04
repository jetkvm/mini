idf_component_register(SRCS "main.c" "../../../password_pbkdf2.c"
    INCLUDE_DIRS "../../../include"
    REQUIRES mbedtls esp_timer)
