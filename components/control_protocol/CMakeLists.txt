idf_component_register(
    SRCS "jetkvm_control.c"
         "jetkvm_hidrpc.c"
         "rpc_video.c"
         "rpc_network.c"
         "rpc_display_settings.c"
         "rpc_system.c"
         "rpc_ota.c"
         "rpc_tls.c"
         "rpc_extra_hostnames.c"
         "rpc_tailscale.c"
         "rpc_mqtt.c"
         "rpc_macros.c"
         "rpc_wol.c"
         "rpc_usb.c"
         "rpc_virtual_media.c"
         "rpc_os_services.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
    PRIV_REQUIRES jetkvm_virtual_media
)
