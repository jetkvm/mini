# TC358743 HDMI -> MIPI-CSI bridge driver.
# No tests/: every entry point programs real TC358743/CSI hardware over I2C.
idf_component_register(
    SRCS "jetkvm_hdmi_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_isp esp_driver_cam jetkvm_settings
    PRIV_REQUIRES esp_driver_gpio esp_hw_support soc esp_rom
)
