set(peer_sources
    src/esp_peer.c
    src/media_lib_weak.c
    src/peer_utils.c
    src/srtp_fail_closed.c
    src/dtls_fingerprint.c
    src/transport/udp.c
    src/transport/tcp.c
    src/transport/tls.c
    src/transport/peer_tls_esp.c
    src/core/agent.c
    src/core/ice.c
    src/core/stun.c
    src/core/rtcp.c
    src/core/rtp.c
    src/core/rtp_jpeg.c
    src/core/twcc.c
    src/core/sdp.c
    src/core/rtp_rolling_buffer.c
    src/core/utils.c
    src/core/buf_mngr.c
    src/core/sctp.c
    src/core/peer_default.c
    src/core/rtp_jitter.c)

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    list(APPEND peer_sources src/dtls_srtp_v6.c)
else()
    list(APPEND peer_sources src/dtls_srtp.c)
endif()

idf_component_register(
    SRCS ${peer_sources}
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS src src/transport src/core
    PRIV_REQUIRES mbedtls esp-tls esp_netif esp_timer esp_ringbuf lwip espressif__esp_libsrtp)

# Packet fields and the application adapters share the peer's internal layouts.
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-strict-aliasing)

# Drop failed encryption before packets reach either the wire or NACK history.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    -Wl,--wrap=agent_send
    -Wl,--wrap=agent_send_high_priority
    -Wl,--wrap=agent_send_to_port
    -Wl,--wrap=rtp_rolling_buffer_insert)
