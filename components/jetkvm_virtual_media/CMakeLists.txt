idf_component_register(
    SRCS "jetkvm_virtual_media.c" "tf_sd_trace.c" "http_range_client.c"
         "http_range_contract.c" "storage_download.c"
         "storage_download_contract.c" "storage_format_contract.c"
         "tf_detect_contract.c" "builtin_image.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs
    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_driver_sdmmc esp_http_client esp_hw_support esp_timer
                  jetkvm_board_config jetkvm_memory_policy mbedtls sdmmc
)

# The installer disc links to the download for this firmware version. The
# version file changes only with the version, so the disc is made again
# when the version or the generator changes.
idf_build_get_property(_jetkvm_project_ver PROJECT_VER)
set(_builtin_installer_version "${CMAKE_CURRENT_BINARY_DIR}/builtin_installer_version.txt")
set(_builtin_installer_source "${CMAKE_CURRENT_BINARY_DIR}/builtin_installer_image.c")
file(CONFIGURE OUTPUT "${_builtin_installer_version}" CONTENT "${_jetkvm_project_ver}")
add_custom_command(
    OUTPUT "${_builtin_installer_source}"
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/builtin_installer_iso.py"
            --version "${_jetkvm_project_ver}"
            --c-output "${_builtin_installer_source}"
    MAIN_DEPENDENCY "${CMAKE_CURRENT_LIST_DIR}/builtin_installer_iso.py"
    DEPENDS "${_builtin_installer_version}"
    COMMENT "Generating the built-in installer disc"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${_builtin_installer_source}")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

# GPIO39..44 can use ESP32-P4 slot 0's dedicated IO_MUX path or slot 1 through
# the GPIO matrix. Keep the host slot selectable until the board's VDDPST_5 /
# SD_VREF wiring has been qualified on hardware.
if(JETKVM_BOARD_VARIANT STREQUAL "wifi")
    set(_jetkvm_tf_sdmmc_default_slot "0")
else()
    set(_jetkvm_tf_sdmmc_default_slot "1")
endif()
set(JETKVM_TF_SDMMC_SLOT "${_jetkvm_tf_sdmmc_default_slot}" CACHE STRING
    "TF-card native SDMMC host slot (0 dedicated IO_MUX, 1 GPIO matrix)")
set(JETKVM_TF_SDMMC_FREQ_KHZ "40000" CACHE STRING
    "TF-card native SDMMC clock in kHz")
set(JETKVM_TF_SDMMC_WIDTH "4" CACHE STRING
    "TF-card native SDMMC bus width (1 or 4)")
set(JETKVM_TF_INTERNAL_PULLUPS "0" CACHE STRING
    "Enable ESP32-P4 internal pull-ups on TF CMD/DAT0/DAT1/DAT2")
set(JETKVM_TF_POWER_OFF_MS "500" CACHE STRING
    "TF-card powered-off discharge interval in milliseconds")
set(JETKVM_TF_BOOT_SELFTEST "0" CACHE STRING
    "Run destructive temporary TF write/read SHA-256 benchmark at boot")
set(JETKVM_TF_BOOT_SELFTEST_MIB "32" CACHE STRING
    "Temporary TF benchmark file size in MiB")
set(JETKVM_TF_RAW_BOOT_SELFTEST "0" CACHE STRING
    "Run read-only raw TF benchmark at boot without mounting a filesystem")
set(JETKVM_TF_RAW_SELFTEST_MIB "512" CACHE STRING
    "Read-only raw TF benchmark size in MiB (0 reads the whole card)")
set(JETKVM_TF_RAW_SELFTEST_CHUNK_KIB "64" CACHE STRING
    "Read-only raw TF benchmark request size in KiB")
set(JETKVM_TF_RAW_DELAY_PHASE "-1" CACHE STRING
    "Raw TF benchmark DDR input phase (-1 board policy, or physical phase 0..3)")
set(JETKVM_TF_RAW_RETRIES "2" CACHE STRING
    "Retries of the exact failed raw TF read request")
set(JETKVM_TF_RMT_TRACE "0" CACHE STRING
    "Capture TF CLK/CMD with passive RMT RX during native initialization")
set(JETKVM_TF_Q1_RC_TEST "0" CACHE STRING
    "Run the TF Q1 powered/off CMD RC diagnostic at boot")
set(JETKVM_TF_MSC_PREFETCH "1" CACHE STRING
    "Pipeline local TF reads into the shared 16 KiB DMA scratch")
set(JETKVM_TF_HTTP_PSRAM_DIRECT "1" CACHE STRING
    "Write aligned HTTP upload buffers directly through SDMMC")
set(JETKVM_TF_UPLOAD_COPY_PIPELINE "0" CACHE STRING
    "Overlap PSRAM-to-internal upload copies with SDMMC writes")
set(JETKVM_TF_SD_ACMD23 "0" CACHE STRING
    "Send SD ACMD23 pre-erase hints before native multi-block writes")

if(NOT JETKVM_TF_SDMMC_SLOT MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_SDMMC_SLOT must be 0 or 1")
endif()
if(NOT JETKVM_TF_SDMMC_FREQ_KHZ MATCHES "^[0-9]+$")
    message(FATAL_ERROR "JETKVM_TF_SDMMC_FREQ_KHZ must be an integer")
endif()
if((JETKVM_TF_SDMMC_FREQ_KHZ LESS 400 OR JETKVM_TF_SDMMC_FREQ_KHZ GREATER 40000) AND
   NOT JETKVM_TF_SDMMC_FREQ_KHZ STREQUAL "50000" AND
   NOT JETKVM_TF_SDMMC_FREQ_KHZ STREQUAL "100000" AND
   NOT JETKVM_TF_SDMMC_FREQ_KHZ STREQUAL "200000")
    message(FATAL_ERROR
        "JETKVM_TF_SDMMC_FREQ_KHZ must be 400..40000, 50000, 100000 or 200000")
endif()
if(NOT JETKVM_TF_SDMMC_WIDTH MATCHES "^(1|4)$")
    message(FATAL_ERROR "JETKVM_TF_SDMMC_WIDTH must be 1 or 4")
endif()
if(NOT JETKVM_TF_INTERNAL_PULLUPS MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_INTERNAL_PULLUPS must be 0 or 1")
endif()
if(NOT JETKVM_TF_POWER_OFF_MS MATCHES "^[0-9]+$" OR
   JETKVM_TF_POWER_OFF_MS LESS 100 OR JETKVM_TF_POWER_OFF_MS GREATER 10000)
    message(FATAL_ERROR "JETKVM_TF_POWER_OFF_MS must be 100..10000")
endif()
if(NOT JETKVM_TF_BOOT_SELFTEST MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_BOOT_SELFTEST must be 0 or 1")
endif()
if(NOT JETKVM_TF_BOOT_SELFTEST_MIB MATCHES "^[0-9]+$" OR
   JETKVM_TF_BOOT_SELFTEST_MIB LESS 1 OR JETKVM_TF_BOOT_SELFTEST_MIB GREATER 512)
    message(FATAL_ERROR "JETKVM_TF_BOOT_SELFTEST_MIB must be 1..512")
endif()
if(NOT JETKVM_TF_RAW_BOOT_SELFTEST MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_RAW_BOOT_SELFTEST must be 0 or 1")
endif()
if(NOT JETKVM_TF_RAW_SELFTEST_MIB MATCHES "^[0-9]+$" OR
   JETKVM_TF_RAW_SELFTEST_MIB GREATER 4096)
    message(FATAL_ERROR
        "JETKVM_TF_RAW_SELFTEST_MIB must be 0 (whole card) or 1..4096")
endif()
if(NOT JETKVM_TF_RAW_SELFTEST_CHUNK_KIB MATCHES "^[0-9]+$" OR
   JETKVM_TF_RAW_SELFTEST_CHUNK_KIB LESS 1 OR
   JETKVM_TF_RAW_SELFTEST_CHUNK_KIB GREATER 256)
    message(FATAL_ERROR "JETKVM_TF_RAW_SELFTEST_CHUNK_KIB must be 1..256")
endif()
if(NOT JETKVM_TF_RAW_DELAY_PHASE MATCHES "^(-1|[0-3])$")
    message(FATAL_ERROR "JETKVM_TF_RAW_DELAY_PHASE must be -1 or 0..3")
endif()
if(NOT JETKVM_TF_RAW_RETRIES MATCHES "^[0-9]+$" OR
   JETKVM_TF_RAW_RETRIES GREATER 10)
    message(FATAL_ERROR "JETKVM_TF_RAW_RETRIES must be 0..10")
endif()
if(JETKVM_TF_BOOT_SELFTEST AND JETKVM_TF_RAW_BOOT_SELFTEST)
    message(FATAL_ERROR
        "Filesystem and raw TF boot self-tests cannot be enabled together")
endif()
if(NOT JETKVM_TF_RMT_TRACE MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_RMT_TRACE must be 0 or 1")
endif()
if(NOT JETKVM_TF_Q1_RC_TEST MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_Q1_RC_TEST must be 0 or 1")
endif()
if(NOT JETKVM_TF_MSC_PREFETCH MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_MSC_PREFETCH must be 0 or 1")
endif()
if(NOT JETKVM_TF_HTTP_PSRAM_DIRECT MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_HTTP_PSRAM_DIRECT must be 0 or 1")
endif()
if(NOT JETKVM_TF_UPLOAD_COPY_PIPELINE MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_UPLOAD_COPY_PIPELINE must be 0 or 1")
endif()
if(NOT JETKVM_TF_SD_ACMD23 MATCHES "^[01]$")
    message(FATAL_ERROR "JETKVM_TF_SD_ACMD23 must be 0 or 1")
endif()
if(JETKVM_TF_UPLOAD_COPY_PIPELINE AND NOT JETKVM_TF_MSC_PREFETCH)
    message(FATAL_ERROR
        "JETKVM_TF_UPLOAD_COPY_PIPELINE reuses the MSC prefetch worker")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JETKVM_TF_SDMMC_SLOT=${JETKVM_TF_SDMMC_SLOT}
    JETKVM_TF_SDMMC_FREQ_KHZ=${JETKVM_TF_SDMMC_FREQ_KHZ}
    JETKVM_TF_SDMMC_WIDTH=${JETKVM_TF_SDMMC_WIDTH}
    JETKVM_TF_INTERNAL_PULLUPS=${JETKVM_TF_INTERNAL_PULLUPS}
    JETKVM_TF_POWER_OFF_MS=${JETKVM_TF_POWER_OFF_MS}
    JETKVM_TF_BOOT_SELFTEST=${JETKVM_TF_BOOT_SELFTEST}
    JETKVM_TF_BOOT_SELFTEST_MIB=${JETKVM_TF_BOOT_SELFTEST_MIB}
    JETKVM_TF_RAW_BOOT_SELFTEST=${JETKVM_TF_RAW_BOOT_SELFTEST}
    JETKVM_TF_RAW_SELFTEST_MIB=${JETKVM_TF_RAW_SELFTEST_MIB}
    JETKVM_TF_RAW_SELFTEST_CHUNK_KIB=${JETKVM_TF_RAW_SELFTEST_CHUNK_KIB}
    JETKVM_TF_RAW_DELAY_PHASE=${JETKVM_TF_RAW_DELAY_PHASE}
    JETKVM_TF_RAW_RETRIES=${JETKVM_TF_RAW_RETRIES}
    JETKVM_TF_RMT_TRACE=${JETKVM_TF_RMT_TRACE}
    JETKVM_TF_Q1_RC_TEST=${JETKVM_TF_Q1_RC_TEST}
    JETKVM_TF_MSC_PREFETCH=${JETKVM_TF_MSC_PREFETCH}
    JETKVM_TF_HTTP_PSRAM_DIRECT=${JETKVM_TF_HTTP_PSRAM_DIRECT}
    JETKVM_TF_UPLOAD_COPY_PIPELINE=${JETKVM_TF_UPLOAD_COPY_PIPELINE}
    JETKVM_TF_SD_ACMD23=${JETKVM_TF_SD_ACMD23}
)

# Bench builds only (see JETKVM_MOCK_CLOUD_JWKS_URL in the top-level
# CMakeLists.txt): a test can take the TF card out and put it back
# (jetkvm_virtual_media_test_card_out, POST /storage/diagnostics/card).
if(JETKVM_MOCK_CLOUD_JWKS_URL)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC JETKVM_TF_CARD_TEST=1)
endif()
