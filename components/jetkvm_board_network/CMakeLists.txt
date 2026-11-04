idf_component_register(
    SRCS "jetkvm_board_network.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_app_format esp_eth esp_event esp_netif esp_system
        espressif__ip101 freertos heap jetkvm_cloud_service
        jetkvm_device_state jetkvm_network_settings jetkvm_ethernet_stats jetkvm_screen
        jetkvm_usb_debugging jetkvm_usb_hid jetkvm_video lwip webrtc_transport
)

set(JETKVM_EMAC_RX_TASK_PRIORITY "19" CACHE STRING
    "ESP EMAC RX task priority; use 19 for drain-first or 18 for tcpip round-robin A/B")
set_property(CACHE JETKVM_EMAC_RX_TASK_PRIORITY PROPERTY STRINGS 19 18)
if(NOT JETKVM_EMAC_RX_TASK_PRIORITY MATCHES "^[0-9]+$")
    message(FATAL_ERROR "JETKVM_EMAC_RX_TASK_PRIORITY must be an integer")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JETKVM_EMAC_RX_TASK_PRIORITY=${JETKVM_EMAC_RX_TASK_PRIORITY})
