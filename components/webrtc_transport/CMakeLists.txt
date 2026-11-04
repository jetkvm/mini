set(JETKVM_HTTP_SERVER_TASK_PRIORITY "5" CACHE STRING
    "FreeRTOS priority for the ESP HTTP server task")
if(NOT JETKVM_HTTP_SERVER_TASK_PRIORITY MATCHES "^[0-9]+$" OR
   JETKVM_HTTP_SERVER_TASK_PRIORITY LESS 1 OR
   JETKVM_HTTP_SERVER_TASK_PRIORITY GREATER 24)
    message(FATAL_ERROR
        "JETKVM_HTTP_SERVER_TASK_PRIORITY must be in the range 1..24")
endif()

idf_component_register(
    SRCS "webrtc_transport.c" "webrtc_http.c" "webrtc_websocket.c" "websocket_origin.c" "peer_udp_timeout_override.c"
         "playout_delay.c" "h264_profile.c" "controlled_session_policy.c" "adaptive_bitrate.c" "rpc_events.c"
         "reboot_flow.c" "peer_memory_arena.c" "http_candidate_policy.c"
         "network_timing.c" "network_timing_wrappers.c"
         "webrtc_transport_rpc_adapters.c" "webrtc_stats_json.c"
         "webrtc_sessions.c" "webrtc_data_channels.c" "data_channel_queue.c" "guest_channels.c" "rtcp_feedback.c" "rpc_worker.c"
         "peer_sctp_hid.c" "peer_sctp_reconfig.c" "peer_sctp_receive.c" "peer_ice_reflexive.c"
         "diagnostics.c" "diagnostics_zip.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_https_server jetkvm_access esp_app_format esp_eth esp_http_server esp_netif esp_ringbuf esp_timer esp_system heap lwip mbedtls
             jetkvm_factory_reset cloud_signaling control_protocol device_api jetkvm_cloud_registration espressif__cjson espressif__esp_libsrtp
             esp_peer jetkvm_device_state jetkvm_ethernet_stats jetkvm_hid_input jetkvm_hid_profiles jetkvm_memory_policy
             jetkvm_audio jetkvm_screen jetkvm_settings jetkvm_network_settings jetkvm_c5_wifi jetkvm_usb_service jetkvm_video_quality
             jetkvm_ota jetkvm_virtual_media jetkvm_virtual_media_http jetkvm_wol web_app
    EMBED_TXTFILES "acceptance_page.html"
    LDFRAGMENTS "linker.lf"
    PRIV_REQUIRES esp_mm jetkvm_board_config jetkvm_flash_read_mode
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JETKVM_HTTP_SERVER_TASK_PRIORITY=${JETKVM_HTTP_SERVER_TASK_PRIORITY}
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,jetkvm_sctp_get_frame"
    "-Wl,-u,jetkvm_ice_authenticated_pair"
    "-Wl,-u,jetkvm_ice_server_recv"
    "-Wl,-u,jetkvm_sctp_handle_forward_tsn"
    "-Wl,-u,jetkvm_sctp_handle_recfg"
    "-Wl,-u,jetkvm_sctp_send_recfg"
    "-Wl,-u,jetkvm_sctp_get_stream"
    "-Wl,-u,jetkvm_sctp_receive_data"
    "-Wl,-u,jetkvm_sctp_receive_window"
    "-Wl,--wrap=srtp_protect"
    "-Wl,--wrap=srtp_unprotect_rtcp"
    "-Wl,--wrap=srtp_cipher_encrypt"
    "-Wl,--wrap=lwip_sendto"
    "-Wl,--wrap=udp_input"
    "-Wl,--wrap=udp_socket_recvfrom_nowait"
    "-Wl,--wrap=esp_eth_transmit"
)
