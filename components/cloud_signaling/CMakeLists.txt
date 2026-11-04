idf_component_register(
    SRCS "jetkvm_cloud_signal.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__cjson mbedtls
)
