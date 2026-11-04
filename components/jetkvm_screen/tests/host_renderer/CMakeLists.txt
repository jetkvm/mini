cmake_minimum_required(VERSION 3.16)

project(jetkvm_ui_renderer LANGUAGES C CXX)

get_filename_component(COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(REPO_DIR "${COMPONENT_DIR}/../.." ABSOLUTE)
set(LVGL_DIR "${REPO_DIR}/managed_components/lvgl__lvgl")

set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)

add_subdirectory("${LVGL_DIR}" "${CMAKE_BINARY_DIR}/lvgl")

set(UI_DIR "${COMPONENT_DIR}/ui")
add_executable(jetkvm_ui_renderer
    render_ui_screens.c
    "${COMPONENT_DIR}/jetkvm_screen_fit.c"
    "${COMPONENT_DIR}/jetkvm_screen_usb_debugging.c"
    "${COMPONENT_DIR}/jetkvm_screen_regulatory.c"
    "${COMPONENT_DIR}/regulatory.c"
    "${UI_DIR}/actions.c"
    "${UI_DIR}/images.c"
    "${UI_DIR}/screens.c"
    "${UI_DIR}/styles.c"
    "${UI_DIR}/ui.c"
    "${UI_DIR}/vars.c"
    "${UI_DIR}/fonts/ui_font_font_bold30.c"
    "${UI_DIR}/fonts/ui_font_font_book16.c"
    "${UI_DIR}/fonts/ui_font_font_book18.c"
    "${UI_DIR}/fonts/ui_font_font_book20.c"
    "${UI_DIR}/fonts/ui_font_font_book24.c"
    "${UI_DIR}/images/ui_image_arrow_icon.c"
    "${UI_DIR}/images/ui_image_back_caret.c"
    "${UI_DIR}/images/ui_image_back_icon.c"
    "${UI_DIR}/images/ui_image_boot_logo_2.c"
    "${UI_DIR}/images/ui_image_check_icon.c"
    "${UI_DIR}/images/ui_image_cloud.c"
    "${UI_DIR}/images/ui_image_cloud_disconnected.c"
    "${UI_DIR}/images/ui_image_d2.c"
    "${UI_DIR}/images/ui_image_ethernet.c"
    "${UI_DIR}/images/ui_image_hdmi.c"
    "${UI_DIR}/images/ui_image_jetkvm.c"
    "${UI_DIR}/images/ui_image_logo.c"
    "${COMPONENT_DIR}/assets/setup_browser.c"
    "${UI_DIR}/images/ui_image_usb.c"
    "${UI_DIR}/images/ui_image_x_icon.c"
)

target_include_directories(jetkvm_ui_renderer PRIVATE
    "${COMPONENT_DIR}"
    "${UI_DIR}"
    "${REPO_DIR}/components/jetkvm_board_config/include"
)

set(JETKVM_BOARD_VARIANT "wifi" CACHE STRING "Board UI variant")
if(JETKVM_BOARD_VARIANT STREQUAL "wifi")
    target_compile_definitions(jetkvm_ui_renderer PRIVATE
        JETKVM_BOARD_WIFI=1 JETKVM_BOARD_ETHERNET=0)
elseif(JETKVM_BOARD_VARIANT STREQUAL "ethernet")
    target_compile_definitions(jetkvm_ui_renderer PRIVATE
        JETKVM_BOARD_WIFI=0 JETKVM_BOARD_ETHERNET=1)
else()
    message(FATAL_ERROR "Unsupported JETKVM_BOARD_VARIANT: ${JETKVM_BOARD_VARIANT}")
endif()
target_link_libraries(jetkvm_ui_renderer PRIVATE lvgl::lvgl m)
target_compile_options(jetkvm_ui_renderer PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wno-unused-parameter
)
