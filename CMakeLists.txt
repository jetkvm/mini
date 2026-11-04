# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# This repository only targets ESP32-P4. Pin the chip in project code so a
# fresh build directory cannot silently fall back to ESP32, and so an ambient
# IDF_TARGET value cannot select a different SoC.
set(IDF_TARGET "esp32p4" CACHE STRING "ESP-IDF target chip" FORCE)

set(JETKVM_BOARD_VARIANT "ethernet" CACHE STRING
    "JetKVM hardware variant (ethernet or wifi)")
set_property(CACHE JETKVM_BOARD_VARIANT PROPERTY STRINGS ethernet wifi)
if(NOT JETKVM_BOARD_VARIANT MATCHES "^(ethernet|wifi)$")
    message(FATAL_ERROR
        "JETKVM_BOARD_VARIANT must be ethernet or wifi, got: "
        "${JETKVM_BOARD_VARIANT}")
endif()

# Keep hardware-specific Kconfig choices outside the common sdkconfig. A fresh
# build directory is required when switching variants because sdkconfig is a
# generated, per-build artifact.
if(NOT SDKCONFIG_DEFAULTS)
    list(APPEND SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()
list(APPEND SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${JETKVM_BOARD_VARIANT}.defaults")
message(STATUS "JetKVM board variant: ${JETKVM_BOARD_VARIANT}")

include(cmake/apply_idf_patch.cmake)
apply_idf_patch(patches/esp-idf-6.0.2-http-server-large-body.patch)
apply_idf_patch(patches/esp-idf-6.0.2-http-server-ws-close-lifetime.patch)
apply_idf_patch(patches/esp-idf-6.0.2-http-server-lru-websocket.patch)
apply_idf_patch(patches/esp-idf-6.0.2-http-server-lru-unanswered.patch)
apply_idf_patch(patches/esp-idf-6.0.2-http-server-single-send-head.patch)
set(JETKVM_TF_SD_ACMD23 "0" CACHE STRING
    "Send SD ACMD23 pre-erase hints before native multi-block writes")
if(NOT JETKVM_TF_SD_ACMD23 MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_SD_ACMD23 must be 0 or 1")
endif()
set(JETKVM_TF_HTTP_PSRAM_DIRECT "1" CACHE STRING
    "Write aligned HTTP upload buffers directly through SDMMC")
if(NOT JETKVM_TF_HTTP_PSRAM_DIRECT MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_HTTP_PSRAM_DIRECT must be 0 or 1")
endif()
set(JETKVM_FATFS_SEQ_WRITE_COALESCE "0" CACHE STRING
    "Coalesce append-only exFAT writes across newly reserved contiguous clusters")
if(NOT JETKVM_FATFS_SEQ_WRITE_COALESCE MATCHES "^[01]$")
    message(FATAL_ERROR
        "JETKVM_FATFS_SEQ_WRITE_COALESCE must be 0 or 1")
endif()
set(JETKVM_FATFS_SEQ_PREERASE "0" CACHE STRING
    "ERASE the aligned free extent found for a new sequential exFAT upload")
if(NOT JETKVM_FATFS_SEQ_PREERASE MATCHES "^[01]$")
    message(FATAL_ERROR
        "JETKVM_FATFS_SEQ_PREERASE must be 0 or 1")
endif()
apply_idf_patch(patches/esp-idf-6.0.2-fatfs-exfat-config.patch)
apply_idf_patch(patches/esp-idf-6.0.2-fatfs-close-retry.patch)
apply_idf_patch(patches/esp-idf-6.0.2-fatfs-sequential-coalesce.patch)
apply_idf_patch(patches/esp-idf-6.0.2-fatfs-sequential-preerase.patch)
apply_idf_patch(patches/esp-idf-6.0.2-spi-private-rx-oom.patch)
apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-aligned-buffer-lifetime.patch)
apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-shared-controller.patch)
apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-isr-slot-dispatch.patch)
apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-tx-cache-sync.patch)
apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-timeout-recovery.patch)
apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-data-error-recovery.patch)
apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-uhs-tuning-modes.patch)
if(JETKVM_TF_SD_ACMD23)
    apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-acmd23-preerase.patch)
endif()
apply_idf_patch(patches/esp-idf-6.0.2-lwip-large-receive-window.patch)
# The WireGuard interface of the Tailscale client keeps its driver state in
# netif->state; ESP-NETIF then keeps its handle in a client-data slot.
apply_idf_patch(patches/esp-idf-6.0.2-lwip-raw-netif-state.patch)
# Backport from ESP-IDF release/v6.0: frees the CA subject copies that the
# cross-signed bundle callback leaks on every TLS handshake.
apply_idf_patch(patches/esp-idf-6.0.2-crt-bundle-cross-signed-leak.patch)
# Backport from ESP-IDF release/v6.0 (70526a29): the certificate bundle no
# longer trusts the emSign roots whose intermediates had weak keys.
apply_idf_patch(patches/esp-idf-6.0.2-crt-bundle-emsign-distrust.patch)
# Hardware AES-GCM for TLS and DTLS records in PSRAM, not only internal RAM.
apply_idf_patch(patches/esp-idf-6.0.2-aes-gcm-psram-dma.patch)
# The GCM DMA path frees its buffers when a later allocation fails.
apply_idf_patch(patches/esp-idf-6.0.2-aes-dma-alloc-failure-cleanup.patch)
# Backport from ESP-IDF release/v6.0: EMAC DMA status bits, cache invalidation
# of RX descriptors, and a check that CONFIG_ETH_DMA_BUFFER_SIZE is a multiple
# of the cache line.
apply_idf_patch(patches/esp-idf-6.0.2-emac-dma-status.patch)
include(cmake/configure_lwip_tcpip_mbox.cmake)
include(cmake/configure_usb_audio.cmake)
# third_party/microlink is exported from jetkvm/microlink at 80d0f8aee5; make
# changes there and export them again.
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/third_party/microlink/components"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp_peer")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
# OTA compares this SemVer against release metadata. Cached so release
# tooling can override it with -DPROJECT_VER=...
set(PROJECT_VER "0.5.0-p4.1" CACHE STRING "Firmware version for getLocalVersion/OTA")

# Test builds only: point the firmware at the e2e mock cloud
# (frontend/jetkvm-ui/e2e/mini/mock-cloud.ts). Its OIDC keys come from this
# URL, http:// cloud URLs are accepted, the mock's test CA is trusted
# (sdkconfig.mockcloud.defaults) and the version gets a "mockcloud" tag.
# release_r2.sh passes an empty value.
set(JETKVM_MOCK_CLOUD_JWKS_URL "" CACHE STRING
    "JWKS URL of the e2e mock cloud (bench e2e builds)")
if(JETKVM_MOCK_CLOUD_JWKS_URL)
    if(NOT JETKVM_MOCK_CLOUD_JWKS_URL MATCHES "^https?://[!-~]+$" OR
       JETKVM_MOCK_CLOUD_JWKS_URL MATCHES "[\"\\\\]")
        message(FATAL_ERROR "JETKVM_MOCK_CLOUD_JWKS_URL must be an http:// or https:// URL")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.mockcloud.defaults")
    if(PROJECT_VER MATCHES "-")
        set(PROJECT_VER "${PROJECT_VER}.mockcloud")
    else()
        set(PROJECT_VER "${PROJECT_VER}-mockcloud")
    endif()
endif()

# Test builds only: a fixed BLE setup code, logged to the serial console, for
# tools/e2e/wifi_reprovision.py and tools/e2e/p4_wifi_ble_setup_e2e.py. Adds
# +codelog to the version. release_r2.sh passes 0 and refuses an image that
# logs the code.
set(JETKVM_WIFI_SETUP_LOG_CODE "0" CACHE STRING
    "Use a fixed BLE Wi-Fi setup code and log it (bench e2e builds)")
if(NOT JETKVM_WIFI_SETUP_LOG_CODE MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_WIFI_SETUP_LOG_CODE must be 0 or 1")
endif()
if(JETKVM_WIFI_SETUP_LOG_CODE)
    set(PROJECT_VER "${PROJECT_VER}+codelog")
endif()
# esp_app_desc_t.version holds 31 characters.
string(LENGTH "${PROJECT_VER}" _jetkvm_project_ver_length)
if(_jetkvm_project_ver_length GREATER 31)
    message(FATAL_ERROR "PROJECT_VER '${PROJECT_VER}' is longer than 31 characters")
endif()
project(jetkvm-mini)

# Recovery image for offset 0, merged from idf.py's flash_args. A custom
# command rather than IDF's always-stale merge-bin target.
idf_build_get_property(_jetkvm_python PYTHON)
set(_jetkvm_full_image "${CMAKE_BINARY_DIR}/jetkvm-mini-full.bin")
# .bin_timestamp is the declared output behind the app image; the image
# itself is a by-product Ninja does not track.
add_custom_command(OUTPUT "${_jetkvm_full_image}"
    COMMAND ${_jetkvm_python} -m esptool --chip ${IDF_TARGET} merge-bin
            -o "${_jetkvm_full_image}" "@${CMAKE_BINARY_DIR}/flash_args"
    DEPENDS gen_project_binary bootloader partition-table blank_ota_data
            "${CMAKE_BINARY_DIR}/.bin_timestamp"
            "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin"
            "${CMAKE_BINARY_DIR}/ota_data_initial.bin"
            "${CMAKE_BINARY_DIR}/flash_args"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Merging jetkvm-mini-full.bin (recovery image)"
    VERBATIM)
add_custom_target(full-image ALL DEPENDS "${_jetkvm_full_image}")

# Apply to the parser library itself, including builds with an old sdkconfig.
include(cmake/limit_cjson_depth.cmake)
idf_component_get_property(_jetkvm_cjson_lib espressif__cjson COMPONENT_LIB)
jetkvm_limit_cjson_depth(${_jetkvm_cjson_lib})

idf_component_get_property(_jetkvm_fatfs_lib fatfs COMPONENT_LIB)
target_compile_definitions(${_jetkvm_fatfs_lib} PRIVATE
    JETKVM_FATFS_SEQ_WRITE_COALESCE=${JETKVM_FATFS_SEQ_WRITE_COALESCE}
    JETKVM_FATFS_SEQ_PREERASE=${JETKVM_FATFS_SEQ_PREERASE})

if(NOT "${CONFIG_LWIP_TCPIP_RECVMBOX_SIZE}" STREQUAL
       "${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}")
    message(FATAL_ERROR
        "Kconfig produced CONFIG_LWIP_TCPIP_RECVMBOX_SIZE="
        "${CONFIG_LWIP_TCPIP_RECVMBOX_SIZE}, expected "
        "${JETKVM_LWIP_TCPIP_RECVMBOX_SIZE}")
endif()
if(NOT "${CONFIG_LWIP_TCP_RECVMBOX_SIZE}" STREQUAL
       "${JETKVM_LWIP_TCP_RECVMBOX_SIZE}")
    message(FATAL_ERROR
        "Kconfig produced CONFIG_LWIP_TCP_RECVMBOX_SIZE="
        "${CONFIG_LWIP_TCP_RECVMBOX_SIZE}, expected "
        "${JETKVM_LWIP_TCP_RECVMBOX_SIZE}")
endif()
if(NOT "${CONFIG_LWIP_TCP_WND_DEFAULT}" STREQUAL
       "${JETKVM_LWIP_TCP_WND_DEFAULT}")
    message(FATAL_ERROR
        "Kconfig produced CONFIG_LWIP_TCP_WND_DEFAULT="
        "${CONFIG_LWIP_TCP_WND_DEFAULT}, expected "
        "${JETKVM_LWIP_TCP_WND_DEFAULT}")
endif()

# managed_components/ is git-ignored; patch it after dependency resolution.
include(cmake/apply_managed_component_patch.cmake)
apply_managed_component_patch(patches/network_provisioning-1.2.4-cjson-free.patch)
apply_managed_component_patch(patches/esp_hosted-2.12.3-explicit-lifecycle.patch)
apply_managed_component_patch(patches/esp_hosted-2.12.3-psram-task-lifecycle.patch)
apply_managed_component_patch(patches/esp_hosted-2.12.3-sdio-rx-double-buf-ownership.patch)
# An SDIO buffer that the internal DMA heap cannot give waits for memory
# instead of asserting (sdio_drv.c rxbuff, sendbuf).
apply_managed_component_patch(patches/esp_hosted-2.12.3-sdio-buffer-wait.patch)
# A station frame that finds the queue to the slave full is refused, as during
# the slave's TX throttling, instead of holding lwIP's tcpip thread.
apply_managed_component_patch(patches/esp_hosted-2.12.3-sta-tx-no-wait.patch)
# The SDIO read and write tasks hand the bus lock to each other, so a stream
# of receive interrupts cannot starve the write task.
apply_managed_component_patch(patches/esp_hosted-2.12.3-sdio-bus-handoff.patch)
apply_managed_component_patch(patches/network_provisioning-1.2.4-configurable-wifi-storage.patch)
apply_managed_component_patch(patches/network_provisioning-1.2.4-scan-one-channel-per-band-slot.patch)
apply_managed_component_patch(patches/network_provisioning-1.2.4-connect-channel-hint.patch)
apply_managed_component_patch(patches/esp_h264-1.4.0-actual-output-invalidate.patch)
# The external-DB patch below edits lines that max-reference-width adds, so
# with both applied only the external-DB patch reverse-checks.
find_package(Git REQUIRED QUIET)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --whitespace=nowarn
            --ignore-space-change --ignore-whitespace
            "${CMAKE_SOURCE_DIR}/patches/esp_h264-1.4.0-external-db.patch"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _h264_external_db_reverse
    OUTPUT_QUIET
    ERROR_QUIET
)
if(_h264_external_db_reverse EQUAL 0)
    message(STATUS "Managed-component patch already applied: patches/esp_h264-1.4.0-max-reference-width.patch (under external-db)")
else()
    apply_managed_component_patch(patches/esp_h264-1.4.0-max-reference-width.patch)
endif()
apply_managed_component_patch(patches/esp_h264-1.4.0-internal-hotpath.patch)
apply_managed_component_patch(patches/esp_h264-1.4.0-flash-hotpath.patch)
apply_managed_component_patch(patches/esp_h264-1.4.0-error-quiesce.patch)
apply_managed_component_patch(patches/esp_h264-1.4.0-frame-timeout-ms.patch)
# The video layer owns the DB buffer for the device's lifetime.
apply_managed_component_patch(patches/esp_h264-1.4.0-external-db.patch)
# An access unit that overflows its output buffer raises the minimum QP, so
# the next IDR fits instead of overflowing at the same QP every second.
apply_managed_component_patch(patches/esp_h264-1.4.1-overflow-qp.patch)
# These three libSRTP patches stack on the same CMakeLists, so an earlier one
# cannot reverse-check once a later one is applied. Detect the final state.
set(_srtp_component_cmake
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_libsrtp/CMakeLists.txt")
set(_srtp_hw_hmac
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_libsrtp/libsrtp/crypto/hash/hmac_esp_sha1.c")
set(_srtp_patch_stack_complete FALSE)
if(EXISTS "${_srtp_component_cmake}" AND EXISTS "${_srtp_hw_hmac}")
    file(READ "${_srtp_component_cmake}" _srtp_component_cmake_text)
    if(_srtp_component_cmake_text MATCHES "aes_icm_mbedtls\\.c" AND
       _srtp_component_cmake_text MATCHES "hmac_esp_sha1\\.c" AND
       _srtp_component_cmake_text MATCHES "PRIV_REQUIRES heap mbedtls esp_hal_security" AND
       _srtp_component_cmake_text MATCHES "PRIVATE ESP_AES")
        set(_srtp_patch_stack_complete TRUE)
    endif()
endif()
if(_srtp_patch_stack_complete)
    message(STATUS "Managed-component libSRTP hardware crypto patch stack already applied")
else()
    apply_managed_component_patch(patches/esp_libsrtp-1.0.0-mbedtls-aes-icm.patch)
    apply_managed_component_patch(patches/esp_libsrtp-1.0.0-mbedtls-hmac-sha1.patch)
    apply_managed_component_patch(patches/esp_libsrtp-1.0.0-esp-hw-noalloc-hmac-sha1.patch)
endif()
apply_managed_component_patch(patches/esp_libsrtp-1.0.0-aes-ctr-block-xor.patch)
apply_managed_component_patch(patches/esp_libsrtp-1.0.0-hmac-fused-finish.patch)
apply_managed_component_patch(patches/esp_libsrtp-1.0.0-hmac-polling-update.patch)
include(cmake/configure_srtp_send_opts.cmake)
apply_managed_component_patch(patches/esp_websocket_client-1.8.0-external-task-stack.patch)

apply_managed_component_patch(patches/tinyusb-0.19.0-esp32-remote-wakeup-min-tick.patch)
apply_managed_component_patch(patches/tinyusb-0.19.0-msc-write-cache.patch)
# The DWC2 DMA's cache maintenance rounds to the line size of the buffer's
# memory, so transfer buffers in PSRAM (OS Services) are written back and
# invalidated whole.
apply_managed_component_patch(patches/tinyusb-0.19.0-dwc2-cache-line-by-address.patch)
# A SET_CONFIGURATION that changes the configuration disables the OUT
# endpoints under Global OUT NAK, so a disable left pending cannot keep the
# next packet in the RX FIFO and stop EP0.
apply_managed_component_patch(patches/tinyusb-0.19.0-dwc2-close-all-out-nak.patch)
apply_managed_component_patch(patches/tinyusb-0.19.0-msc-async-read10-in-task.patch)
apply_managed_component_patch(patches/esp_tinyusb-2.2.1-custom-msc-backend.patch)
# That patch adds CONFIG_TINYUSB_MSC_CUSTOM_BACKEND. A fresh build directory
# parses Kconfig before the patch exists, so its generated sdkconfig lacks the
# symbol and sdkconfig.defaults never re-applies. Remove that sdkconfig and
# stop; the next configure regenerates it with the symbol.
if(NOT CONFIG_TINYUSB_MSC_CUSTOM_BACKEND)
    file(REMOVE "${SDKCONFIG}")
    message(FATAL_ERROR
        "sdkconfig predates the TinyUSB MSC patch; removed ${SDKCONFIG}. "
        "Run the build again.")
endif()
