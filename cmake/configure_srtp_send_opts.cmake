# None of these changes the SRTP profile, RTP size or frame buffering.
option(JETKVM_SRTP_AES_BLOCK_XOR "Use whole-block XOR in SRTP AES CTR" ON)
option(JETKVM_SRTP_HMAC_FUSED_FINISH "Finish SRTP HMAC in one SHA hardware session" ON)
option(JETKVM_SRTP_HMAC_POLLING_UPDATE "Use register-fed SRTP HMAC body updates" ON)

idf_component_get_property(_jetkvm_srtp_lib espressif__esp_libsrtp COMPONENT_LIB)
target_compile_definitions(${_jetkvm_srtp_lib} PRIVATE
    JETKVM_SRTP_AES_BLOCK_XOR=$<BOOL:${JETKVM_SRTP_AES_BLOCK_XOR}>
    JETKVM_SRTP_HMAC_FUSED_FINISH=$<BOOL:${JETKVM_SRTP_HMAC_FUSED_FINISH}>
    JETKVM_SRTP_HMAC_POLLING_UPDATE=$<BOOL:${JETKVM_SRTP_HMAC_POLLING_UPDATE}>)
