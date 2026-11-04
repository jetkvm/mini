idf_component_register(INCLUDE_DIRS "include")

if(JETKVM_BOARD_VARIANT STREQUAL "wifi")
    target_compile_definitions(${COMPONENT_LIB} INTERFACE
        JETKVM_BOARD_WIFI=1
        JETKVM_BOARD_ETHERNET=0)
elseif(JETKVM_BOARD_VARIANT STREQUAL "ethernet")
    target_compile_definitions(${COMPONENT_LIB} INTERFACE
        JETKVM_BOARD_WIFI=0
        JETKVM_BOARD_ETHERNET=1)
else()
    message(FATAL_ERROR "Unsupported JETKVM_BOARD_VARIANT: ${JETKVM_BOARD_VARIANT}")
endif()
