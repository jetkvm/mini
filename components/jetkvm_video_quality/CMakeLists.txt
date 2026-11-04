idf_component_register(
    SRCS "jetkvm_video_quality.c"
    INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
