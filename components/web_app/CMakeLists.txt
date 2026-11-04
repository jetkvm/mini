get_filename_component(WEB_APP_DEFAULT_DIST_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../frontend/jetkvm-ui/dist-p4" ABSOLUTE)
set(WEB_APP_DIST_DIR "${WEB_APP_DEFAULT_DIST_DIR}" CACHE PATH
    "Directory containing the built device-mode JetKVM frontend")

# ESP-IDF evaluates component CMake files once in script mode to discover
# dependencies. Defer asset discovery and target creation to the real configure
# pass; CONFIGURE_DEPENDS and build targets are not valid during that scan.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES esp_http_server jetkvm_access)
    return()
endif()

# The web assets are embedded into the application image, so every OTA
# artifact carries app and UI as one atomic unit. A missing device frontend is
# fatal: silently falling back to the acceptance page produces a flashable but
# user-facingly incorrect firmware image.
if(EXISTS "${WEB_APP_DIST_DIR}/index.html")
    set(WEB_APP_PACK_BIN "${CMAKE_BINARY_DIR}/web_app_pack.bin")
    set(WEB_APP_PACK_MANIFEST "${CMAKE_BINARY_DIR}/web_app_pack-manifest.json")
    file(GLOB_RECURSE web_app_assets CONFIGURE_DEPENDS LIST_DIRECTORIES false
         "${WEB_APP_DIST_DIR}/*")
    # The pack is embedded in the app image, which must fit the 6 MiB ota_0
    # and ota_1 slots in partitions.csv; the 2.5 MiB pack budget (--max-size)
    # leaves the rest of the slot for the firmware.
    add_custom_command(
        OUTPUT "${WEB_APP_PACK_BIN}" "${WEB_APP_PACK_MANIFEST}"
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/web_app/pack_web_app.py"
                --input "${WEB_APP_DIST_DIR}"
                --output "${WEB_APP_PACK_BIN}"
                --manifest "${WEB_APP_PACK_MANIFEST}"
                --max-size 0x280000
        MAIN_DEPENDENCY "${CMAKE_CURRENT_LIST_DIR}/../../tools/web_app/pack_web_app.py"
        DEPENDS ${web_app_assets}
        COMMENT "Packing JetKVM frontend for app-image embedding"
        VERBATIM)
    add_custom_target(web_app_embedded_pack DEPENDS "${WEB_APP_PACK_BIN}")

    idf_component_register(
        SRCS "web_app.c" "web_app_pack.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "private_include"
        REQUIRES esp_http_server jetkvm_access
    )
    target_add_binary_data(${COMPONENT_LIB} "${WEB_APP_PACK_BIN}" BINARY)
    add_dependencies(${COMPONENT_LIB} web_app_embedded_pack)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        WEB_APP_EMBEDDED_PACK=1)
else()
    message(FATAL_ERROR
        "JetKVM device frontend is missing: '${WEB_APP_DIST_DIR}/index.html'. "
        "Run 'npm run build:device -- --outDir dist-p4' in frontend/jetkvm-ui "
        "with Node 22.21.1 "
        "before building the firmware. The acceptance/debug page "
        "is not a production KVM frontend.")
endif()
