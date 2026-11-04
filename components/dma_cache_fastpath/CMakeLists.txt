idf_component_register(SRCS "dma_cache_fastpath.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_mm freertos)

# Keep the SDK installations immutable.  GNU ld wrapping redirects calls from
# esp_driver_ppa and esp_h264 to this component, while __real_esp_cache_msync
# remains the unmodified IDF implementation and is the fail-safe path.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_cache_msync")
