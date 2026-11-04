set(SRCS
    "src/microlink.c"
    "src/ml_packet_filter.c"
    "src/ml_peer_map.c"
    "src/ml_peer_select.c"
    "src/ml_timestamp.c"
    "src/ml_key_expiry.c"
    "src/ml_coord.c"
    "src/ml_derp.c"
    "src/ml_derp_stream.c"
    "src/ml_net_io.c"
    "src/ml_wg_mgr.c"
    "src/ml_stun.c"
    "src/ml_noise.c"
    "src/ml_h2.c"
    "src/ml_h2_message.c"
    "src/ml_udp.c"
    "src/ml_tcp.c"
    "src/ml_peer_nvs.c"
    "src/nacl_box.c"
    "src/x25519.c"
    "src/ml_zerocopy.c"
    "src/ml_cellular.c"
    "src/ml_at_socket.c"
    "src/ml_net_switch.c"
    "src/ml_config_httpd.c"
)

set(REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    lwip
    mbedtls
    espressif__cjson
    driver
    esp_driver_uart
    esp_driver_gpio
    esp_driver_tsens
    esp_http_server
    esp_http_client
    esp-tls
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES
        wireguard_lwip
)

# At -O2 GCC's inlining lets it see strncpy(dst, src, sizeof(dst) - 1) copy a
# 63-byte string into a 64-byte field and calls that a possible truncation,
# although the terminator is written on the next line every time. Forty such
# copies in this component; a warning per inlining site is noise, not a bug.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-stringop-truncation)

# Compile the pinned project cJSON with private symbols. Its allocations use
# MicroLink's PSRAM hooks; RPC and other firmware keep their own allocator.
idf_component_get_property(_ml_cjson_dir espressif__cjson COMPONENT_DIR)
set(_ml_cjson_source "${CMAKE_CURRENT_BINARY_DIR}/private_cjson.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_cjson.py" "${_ml_cjson_dir}/cJSON/cJSON.c")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_cjson.py"
    "${_ml_cjson_dir}/cJSON/cJSON.c" "${_ml_cjson_source}"
    RESULT_VARIABLE _ml_cjson_result)
if(NOT _ml_cjson_result EQUAL 0)
    message(FATAL_ERROR "Could not prepare the private Tailscale JSON parser")
endif()
target_sources(${COMPONENT_LIB} PRIVATE "${_ml_cjson_source}")
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/include/ml_json_namespace.h")
