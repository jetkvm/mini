#include "mbedtls/base64.h"

#include <stdint.h>

static const unsigned char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int decode_value(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    if (olen == NULL || (src == NULL && slen != 0)) {
        return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
    }
    if (slen > SIZE_MAX - 2 || (slen + 2) / 3 > SIZE_MAX / 4) {
        *olen = SIZE_MAX;
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }
    const size_t required = ((slen + 2) / 3) * 4;
    *olen = required;
    if (required == SIZE_MAX || required + 1 > dlen || dst == NULL) {
        *olen = required == SIZE_MAX ? SIZE_MAX : required + 1;
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }

    size_t input = 0;
    size_t output = 0;
    while (input < slen) {
        const size_t remaining = slen - input;
        const uint32_t a = src[input++];
        const uint32_t b = remaining > 1 ? src[input++] : 0;
        const uint32_t c = remaining > 2 ? src[input++] : 0;
        const uint32_t group = (a << 16U) | (b << 8U) | c;
        dst[output++] = alphabet[(group >> 18U) & 0x3fU];
        dst[output++] = alphabet[(group >> 12U) & 0x3fU];
        dst[output++] = remaining > 1 ? alphabet[(group >> 6U) & 0x3fU] : '=';
        dst[output++] = remaining > 2 ? alphabet[group & 0x3fU] : '=';
    }
    dst[required] = '\0';
    *olen = required;
    return 0;
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    if (olen == NULL || src == NULL || slen == 0 || slen % 4 != 0) {
        return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
    }
    size_t padding = src[slen - 1] == '=' ? 1 : 0;
    if (padding == 1 && src[slen - 2] == '=') {
        padding = 2;
    }
    const size_t required = (slen / 4) * 3 - padding;
    *olen = required;
    if (required > dlen || (required != 0 && dst == NULL)) {
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }

    size_t output = 0;
    for (size_t input = 0; input < slen; input += 4) {
        int values[4] = { 0, 0, 0, 0 };
        for (size_t i = 0; i < 4; ++i) {
            if (src[input + i] == '=' && input + i >= slen - padding) {
                values[i] = 0;
            } else {
                values[i] = decode_value(src[input + i]);
                if (values[i] < 0) {
                    return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
                }
            }
        }
        const uint32_t group = ((uint32_t)values[0] << 18U) |
                               ((uint32_t)values[1] << 12U) |
                               ((uint32_t)values[2] << 6U) |
                               (uint32_t)values[3];
        if (output < required) {
            dst[output++] = (unsigned char)(group >> 16U);
        }
        if (output < required) {
            dst[output++] = (unsigned char)(group >> 8U);
        }
        if (output < required) {
            dst[output++] = (unsigned char)group;
        }
    }
    return 0;
}
