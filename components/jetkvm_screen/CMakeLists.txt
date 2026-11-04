idf_component_register(
    SRCS
        "jetkvm_screen.c"
        "local_display_settings.c"
        "jetkvm_lvgl_memory.c"
        "jetkvm_screen_fit.c"
        "jetkvm_screen_orientation.c"
        "jetkvm_screen_button.c"
        "jetkvm_screen_buttons.c"
        "jetkvm_screen_usb_debugging.c"
        "jetkvm_screen_regulatory.c"
        "regulatory.c"
        "ui/actions.c"
        "ui/images.c"
        "ui/screens.c"
        "ui/styles.c"
        "ui/ui.c"
        "ui/vars.c"
        "ui/fonts/ui_font_font_bold30.c"
        "ui/fonts/ui_font_font_book16.c"
        "ui/fonts/ui_font_font_book18.c"
        "ui/fonts/ui_font_font_book20.c"
        "ui/fonts/ui_font_font_book24.c"
        "ui/images/ui_image_arrow_icon.c"
        "ui/images/ui_image_back_caret.c"
        "ui/images/ui_image_back_icon.c"
        "ui/images/ui_image_boot_logo_2.c"
        "ui/images/ui_image_check_icon.c"
        "ui/images/ui_image_cloud.c"
        "ui/images/ui_image_cloud_disconnected.c"
        "ui/images/ui_image_d2.c"
        "ui/images/ui_image_ethernet.c"
        "ui/images/ui_image_hdmi.c"
        "ui/images/ui_image_jetkvm.c"
        "ui/images/ui_image_logo.c"
        "assets/setup_browser.c"
        "assets/font_bold20.c"
        "ui/images/ui_image_usb.c"
        "ui/images/ui_image_x_icon.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "ui"
    REQUIRES jetkvm_board_config lvgl
    PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_ppa esp_driver_spi esp_lcd esp_timer
                  jetkvm_memory_policy control_protocol nvs_flash
                  jetkvm_factory_reset jetkvm_usb_debugging esp_system)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# LVGL's custom core becomes unresolved only after liblvgl is scanned. Force
# this object out of the component archive independently of static-library
# ordering so every core entry point is present when liblvgl is processed.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=lv_mem_init"
)
