idf_component_register(SRCS "jetkvm_audio.c" INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_audio_codec esp_timer freertos heap nvs_flash jetkvm_memory_policy control_protocol espressif__esp_tinyusb)
