#pragma once
#include <stdbool.h>
#include <stdint.h>
/* RFC 8018, one SHA-256 output block, with a cooperative scheduling hook. */
bool jetkvm_password_pbkdf2(const char *password, const uint8_t salt[16],
    uint32_t iterations, uint8_t output[32], void (*yield)(void));
