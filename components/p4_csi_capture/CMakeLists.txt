idf_component_register(
    SRCS "p4_cam_ctlr_csi.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_driver_cam esp_driver_isp esp_hal_cam esp_mm
    PRIV_REQUIRES esp_driver_gpio esp_driver_dma esp_hw_support
    LDFRAGMENTS "linker.lf"
)

if(CONFIG_PM_ENABLE)
    idf_component_optional_requires(PRIVATE esp_pm)
endif()
