set(JETKVM_TF_HTTP_PSRAM_DIRECT "1" CACHE STRING
    "Shared VM direct-write setting needed during component discovery")
set(JETKVM_HTTP_UPLOAD_WRITER_PRIORITY "6" CACHE STRING
    "FreeRTOS priority for the HTTP TF upload writer task")
set(JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT "1" CACHE STRING
    "Receive sequential HTTP uploads into internal-DMA buffers")
set(JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE "1" CACHE STRING
    "Use two pipelined internal-DMA buffers for sequential HTTP uploads")
set(JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB "24" CACHE STRING
    "Internal-direct pipeline buffer size in KiB (24, 32 or 40)")
if(NOT JETKVM_HTTP_UPLOAD_WRITER_PRIORITY MATCHES "^[0-9]+$" OR
   JETKVM_HTTP_UPLOAD_WRITER_PRIORITY LESS 1 OR
   JETKVM_HTTP_UPLOAD_WRITER_PRIORITY GREATER 24)
    message(FATAL_ERROR
        "JETKVM_HTTP_UPLOAD_WRITER_PRIORITY must be in the range 1..24")
endif()
if(NOT JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT MATCHES "^[01]$")
    message(FATAL_ERROR
        "JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT must be 0 or 1")
endif()
if(NOT JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE MATCHES "^[01]$")
    message(FATAL_ERROR
        "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE must be 0 or 1")
endif()
if(JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE AND
   NOT JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT)
    message(FATAL_ERROR
        "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE requires "
        "JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT=1")
endif()
if(NOT JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB MATCHES "^(24|32|40)$")
    message(FATAL_ERROR
        "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB must be 24, 32 or 40")
endif()
if(JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT AND NOT JETKVM_TF_HTTP_PSRAM_DIRECT)
    message(FATAL_ERROR
        "JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT requires "
        "JETKVM_TF_HTTP_PSRAM_DIRECT=1 so the VM does not allocate a second staging buffer")
endif()

idf_component_register(
    SRCS "jetkvm_virtual_media_http.c" "http_upload_gate.c"
    INCLUDE_DIRS "include"
    REQUIRES jetkvm_access esp_http_server esp_timer espressif__cjson heap soc
             jetkvm_memory_policy jetkvm_virtual_media
    PRIV_REQUIRES jetkvm_board_config sdmmc
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JETKVM_HTTP_UPLOAD_WRITER_PRIORITY=${JETKVM_HTTP_UPLOAD_WRITER_PRIORITY}
    JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT=${JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT}
    JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE=${JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE}
    JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB=${JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB}
)
