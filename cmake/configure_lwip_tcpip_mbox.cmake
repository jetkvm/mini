# lwIP receive mailbox sizes and TCP window, overridable per build directory.
# CONFIG_* values passed to CMake do not override Kconfig, so non-default
# values go into a build-local copy of the sdkconfig.
#
# The Wi-Fi board takes a 64 KiB window: a larger one lets a sender overflow
# the ESP32-C5's buffers, which then drop other traffic with the upload.
if(JETKVM_BOARD_VARIANT STREQUAL "wifi")
    set(_jetkvm_tcp_wnd_board_default "65536")
else()
    set(_jetkvm_tcp_wnd_board_default "1310720")
endif()
set(JETKVM_LWIP_TCPIP_RECVMBOX_SIZE "256" CACHE STRING
    "Global lwIP tcpip_thread input mailbox entries (64/128/256/512/1024)")
set_property(CACHE JETKVM_LWIP_TCPIP_RECVMBOX_SIZE PROPERTY STRINGS
    64 128 256 512 1024)
set(JETKVM_LWIP_TCP_RECVMBOX_SIZE "256" CACHE STRING
    "Per-TCP-netconn receive mailbox entries (128/256/512/1024)")
set_property(CACHE JETKVM_LWIP_TCP_RECVMBOX_SIZE PROPERTY STRINGS 128 256 512 1024)
# Empty JETKVM_LWIP_TCP_WND takes the board's window. The result is a normal
# variable so that a cached JETKVM_LWIP_TCP_WND_DEFAULT cannot shadow it.
set(JETKVM_LWIP_TCP_WND "" CACHE STRING
    "Scaled TCP receive window bytes (65536/1048576/1310720/2097152); empty: the board's")
set_property(CACHE JETKVM_LWIP_TCP_WND PROPERTY STRINGS
    "" 65536 1048576 1310720 2097152)
if(JETKVM_LWIP_TCP_WND STREQUAL "")
    set(JETKVM_LWIP_TCP_WND_DEFAULT "${_jetkvm_tcp_wnd_board_default}")
else()
    set(JETKVM_LWIP_TCP_WND_DEFAULT "${JETKVM_LWIP_TCP_WND}")
endif()

set(_jetkvm_tcpip_mbox_allowed 64 128 256 512 1024)
if(NOT JETKVM_LWIP_TCPIP_RECVMBOX_SIZE IN_LIST
       _jetkvm_tcpip_mbox_allowed)
    message(FATAL_ERROR
        "JETKVM_LWIP_TCPIP_RECVMBOX_SIZE must be one of: "
        "64, 128, 256, 512, 1024")
endif()

set(_jetkvm_tcp_mbox_allowed 128 256 512 1024)
if(NOT JETKVM_LWIP_TCP_RECVMBOX_SIZE IN_LIST _jetkvm_tcp_mbox_allowed)
    message(FATAL_ERROR
        "JETKVM_LWIP_TCP_RECVMBOX_SIZE must be one of: 128, 256, 512, 1024")
endif()

set(_jetkvm_tcp_wnd_allowed 65536 1048576 1310720 2097152)
if(NOT JETKVM_LWIP_TCP_WND_DEFAULT IN_LIST _jetkvm_tcp_wnd_allowed)
    message(FATAL_ERROR
        "JETKVM_LWIP_TCP_WND must be one of: "
        "65536, 1048576, 1310720, 2097152")
endif()

if(DEFINED SDKCONFIG AND NOT "${SDKCONFIG}" STREQUAL "")
    get_filename_component(_jetkvm_tcpip_mbox_base_sdkconfig
        "${SDKCONFIG}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(_jetkvm_tcpip_mbox_base_sdkconfig
        "${CMAKE_SOURCE_DIR}/sdkconfig")
endif()
set(SDKCONFIG "${_jetkvm_tcpip_mbox_base_sdkconfig}")

set(_jetkvm_tcpip_mbox_sdkconfig_text "")
if(EXISTS "${_jetkvm_tcpip_mbox_base_sdkconfig}")
    file(READ "${_jetkvm_tcpip_mbox_base_sdkconfig}"
         _jetkvm_tcpip_mbox_sdkconfig_text)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_jetkvm_tcpip_mbox_base_sdkconfig}")
endif()

string(REGEX MATCH
    "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=([0-9]+)"
    _jetkvm_tcpip_mbox_current_match
    "${_jetkvm_tcpip_mbox_sdkconfig_text}")
set(_jetkvm_tcpip_mbox_current "${CMAKE_MATCH_1}")
set(_jetkvm_lwip_mbox_changed FALSE)

if(NOT "${_jetkvm_tcpip_mbox_current}" STREQUAL
       "${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}")
    if(_jetkvm_tcpip_mbox_current_match)
        string(REGEX REPLACE
            "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=[0-9]+"
            "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}"
            _jetkvm_tcpip_mbox_sdkconfig_text
            "${_jetkvm_tcpip_mbox_sdkconfig_text}")
    elseif(_jetkvm_tcpip_mbox_sdkconfig_text MATCHES
           "CONFIG_TCPIP_RECVMBOX_SIZE=[0-9]+")
        string(REGEX REPLACE
            "CONFIG_TCPIP_RECVMBOX_SIZE=[0-9]+"
            "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}"
            _jetkvm_tcpip_mbox_sdkconfig_text
            "${_jetkvm_tcpip_mbox_sdkconfig_text}")
    else()
        if(NOT "${_jetkvm_tcpip_mbox_sdkconfig_text}" STREQUAL "" AND
           NOT _jetkvm_tcpip_mbox_sdkconfig_text MATCHES "\n$")
            string(APPEND _jetkvm_tcpip_mbox_sdkconfig_text "\n")
        endif()
        string(APPEND _jetkvm_tcpip_mbox_sdkconfig_text
            "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}\n")
    endif()

    set(_jetkvm_lwip_mbox_changed TRUE)
endif()

string(REGEX MATCH
    "CONFIG_LWIP_TCP_RECVMBOX_SIZE=([0-9]+)"
    _jetkvm_tcp_mbox_current_match
    "${_jetkvm_tcpip_mbox_sdkconfig_text}")
set(_jetkvm_tcp_mbox_current "${CMAKE_MATCH_1}")

if(NOT "${_jetkvm_tcp_mbox_current}" STREQUAL
       "${JETKVM_LWIP_TCP_RECVMBOX_SIZE}")
    if(_jetkvm_tcp_mbox_current_match)
        string(REGEX REPLACE
            "CONFIG_LWIP_TCP_RECVMBOX_SIZE=[0-9]+"
            "CONFIG_LWIP_TCP_RECVMBOX_SIZE=${JETKVM_LWIP_TCP_RECVMBOX_SIZE}"
            _jetkvm_tcpip_mbox_sdkconfig_text
            "${_jetkvm_tcpip_mbox_sdkconfig_text}")
    elseif(_jetkvm_tcpip_mbox_sdkconfig_text MATCHES
           "CONFIG_TCP_RECVMBOX_SIZE=[0-9]+")
        string(REGEX REPLACE
            "CONFIG_TCP_RECVMBOX_SIZE=[0-9]+"
            "CONFIG_LWIP_TCP_RECVMBOX_SIZE=${JETKVM_LWIP_TCP_RECVMBOX_SIZE}"
            _jetkvm_tcpip_mbox_sdkconfig_text
            "${_jetkvm_tcpip_mbox_sdkconfig_text}")
    else()
        if(NOT "${_jetkvm_tcpip_mbox_sdkconfig_text}" STREQUAL "" AND
           NOT _jetkvm_tcpip_mbox_sdkconfig_text MATCHES "\n$")
            string(APPEND _jetkvm_tcpip_mbox_sdkconfig_text "\n")
        endif()
        string(APPEND _jetkvm_tcpip_mbox_sdkconfig_text
            "CONFIG_LWIP_TCP_RECVMBOX_SIZE=${JETKVM_LWIP_TCP_RECVMBOX_SIZE}\n")
    endif()

    set(_jetkvm_lwip_mbox_changed TRUE)
endif()

string(REGEX MATCH
    "CONFIG_LWIP_TCP_WND_DEFAULT=([0-9]+)"
    _jetkvm_tcp_wnd_current_match
    "${_jetkvm_tcpip_mbox_sdkconfig_text}")
set(_jetkvm_tcp_wnd_current "${CMAKE_MATCH_1}")

if(NOT "${_jetkvm_tcp_wnd_current}" STREQUAL
       "${JETKVM_LWIP_TCP_WND_DEFAULT}")
    if(_jetkvm_tcp_wnd_current_match)
        string(REGEX REPLACE
            "CONFIG_LWIP_TCP_WND_DEFAULT=[0-9]+"
            "CONFIG_LWIP_TCP_WND_DEFAULT=${JETKVM_LWIP_TCP_WND_DEFAULT}"
            _jetkvm_tcpip_mbox_sdkconfig_text
            "${_jetkvm_tcpip_mbox_sdkconfig_text}")
    elseif(_jetkvm_tcpip_mbox_sdkconfig_text MATCHES
           "CONFIG_TCP_WND_DEFAULT=[0-9]+")
        string(REGEX REPLACE
            "CONFIG_TCP_WND_DEFAULT=[0-9]+"
            "CONFIG_LWIP_TCP_WND_DEFAULT=${JETKVM_LWIP_TCP_WND_DEFAULT}"
            _jetkvm_tcpip_mbox_sdkconfig_text
            "${_jetkvm_tcpip_mbox_sdkconfig_text}")
    else()
        if(NOT "${_jetkvm_tcpip_mbox_sdkconfig_text}" STREQUAL "" AND
           NOT _jetkvm_tcpip_mbox_sdkconfig_text MATCHES "\n$")
            string(APPEND _jetkvm_tcpip_mbox_sdkconfig_text "\n")
        endif()
        string(APPEND _jetkvm_tcpip_mbox_sdkconfig_text
            "CONFIG_LWIP_TCP_WND_DEFAULT=${JETKVM_LWIP_TCP_WND_DEFAULT}\n")
    endif()

    set(_jetkvm_lwip_mbox_changed TRUE)
endif()

if(_jetkvm_lwip_mbox_changed)
    set(_jetkvm_lwip_mbox_overlay
        "${CMAKE_BINARY_DIR}/sdkconfig.jetkvm-lwip-g${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}-t${JETKVM_LWIP_TCP_RECVMBOX_SIZE}-w${JETKVM_LWIP_TCP_WND_DEFAULT}")
    file(WRITE "${_jetkvm_lwip_mbox_overlay}"
         "${_jetkvm_tcpip_mbox_sdkconfig_text}")
    set(SDKCONFIG "${_jetkvm_lwip_mbox_overlay}")
    message(STATUS
        "JetKVM lwIP mailbox A/B override: tcpip="
        "${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}, tcp="
        "${JETKVM_LWIP_TCP_RECVMBOX_SIZE} entries, wnd="
        "${JETKVM_LWIP_TCP_WND_DEFAULT} bytes (${SDKCONFIG})")
else()
    message(STATUS
        "JetKVM lwIP mailboxes: tcpip="
        "${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}, tcp="
        "${JETKVM_LWIP_TCP_RECVMBOX_SIZE} entries, wnd="
        "${JETKVM_LWIP_TCP_WND_DEFAULT} bytes (base sdkconfig)")
endif()

unset(_jetkvm_tcpip_mbox_allowed)
unset(_jetkvm_tcp_mbox_allowed)
unset(_jetkvm_tcp_wnd_allowed)
unset(_jetkvm_tcp_wnd_board_default)
unset(_jetkvm_tcpip_mbox_base_sdkconfig)
unset(_jetkvm_tcpip_mbox_sdkconfig_text)
unset(_jetkvm_tcpip_mbox_current_match)
unset(_jetkvm_tcpip_mbox_current)
unset(_jetkvm_tcp_mbox_current_match)
unset(_jetkvm_tcp_mbox_current)
unset(_jetkvm_tcp_wnd_current_match)
unset(_jetkvm_tcp_wnd_current)
unset(_jetkvm_lwip_mbox_changed)
unset(_jetkvm_lwip_mbox_overlay)
