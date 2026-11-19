idf_component_register(
    SRCS "jetkvm_mdns_core.c" "jetkvm_mdns.c"
    INCLUDE_DIRS "include"
    REQUIRES control_protocol
    PRIV_REQUIRES freertos log
)
