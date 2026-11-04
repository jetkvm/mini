/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "upload_identity_contract.h"

static const char LOWER_DIGEST[] =
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";
static const char UPPER_DIGEST[] =
    "0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF";

int main(void)
{
    uint8_t record[JETKVM_UPLOAD_IDENTITY_RECORD_SIZE] = {0};
    char decoded_digest[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U] = {0};
    uint64_t decoded_size = 0U;

    static const uint8_t crc_vector[] = "123456789";
    assert(jetkvm_upload_identity_crc32(
               crc_vector, sizeof(crc_vector) - 1U) == UINT32_C(0xcbf43926));
    assert(jetkvm_upload_filename_has_suffix_ci(
        "IMAGE.ISO.INCOMPLETE", ".incomplete"));
    assert(jetkvm_upload_filename_has_suffix_ci(
        "IMAGE.ISO.InComplete.Identity", ".incomplete.identity"));
    assert(jetkvm_upload_filename_has_suffix_ci(
        "IMAGE.ISO.INCOMPLETE.IDENTITY.TMP", ".incomplete.identity.tmp"));
    assert(!jetkvm_upload_filename_has_suffix_ci(
        "IMAGE.ISO.COMPLETE", ".incomplete"));
    assert(!jetkvm_upload_filename_has_suffix_ci(NULL, ".incomplete"));

    assert(jetkvm_upload_sha256_hex_valid(LOWER_DIGEST));
    assert(jetkvm_upload_sha256_hex_valid(UPPER_DIGEST));
    assert(jetkvm_upload_sha256_hex_valid(
        JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE));
    assert(!jetkvm_upload_sha256_hex_valid(NULL));
    assert(!jetkvm_upload_sha256_hex_valid("0123"));
    char too_long[66];
    memset(too_long, 'a', sizeof(too_long));
    too_long[65] = '\0';
    assert(!jetkvm_upload_sha256_hex_valid(too_long));
    char non_hex[65];
    memcpy(non_hex, LOWER_DIGEST, sizeof(non_hex));
    non_hex[31] = 'g';
    assert(!jetkvm_upload_sha256_hex_valid(non_hex));

    const uint64_t expected_size = UINT64_C(0xfedcba9876543210);
    assert(jetkvm_upload_identity_encode(expected_size, UPPER_DIGEST,
                                         record));
    assert(jetkvm_upload_identity_decode(record, sizeof(record),
                                         &decoded_size, decoded_digest));
    assert(decoded_size == expected_size);
    assert(strcmp(decoded_digest, LOWER_DIGEST) == 0);
    assert(jetkvm_upload_identity_matches(expected_size, UPPER_DIGEST,
                                          decoded_size, decoded_digest));
    assert(!jetkvm_upload_identity_matches(expected_size - 1U, UPPER_DIGEST,
                                           decoded_size, decoded_digest));

    /* Go-compatible clients omit the digest. Their reserved sentinel may
     * resume a valid v1 sidecar by size, including one written earlier with a
     * real digest. Supplying a real digest remains strict. */
    assert(jetkvm_upload_resume_identity_matches(
        expected_size, JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE,
        decoded_size, decoded_digest));
    assert(!jetkvm_upload_resume_identity_matches(
        expected_size - 1U,
        JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE,
        decoded_size, decoded_digest));
    assert(jetkvm_upload_resume_identity_matches(
        expected_size, UPPER_DIGEST, decoded_size, decoded_digest));
    assert(!jetkvm_upload_resume_identity_matches(
        expected_size, LOWER_DIGEST, decoded_size,
        JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE));

    assert(!jetkvm_upload_identity_decode(record, sizeof(record) - 1U,
                                          &decoded_size, decoded_digest));
    assert(!jetkvm_upload_identity_decode(record, sizeof(record) + 1U,
                                          &decoded_size, decoded_digest));

    uint8_t corrupt[JETKVM_UPLOAD_IDENTITY_RECORD_SIZE];
    for (size_t byte = 0U; byte < sizeof(record); ++byte) {
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            memcpy(corrupt, record, sizeof(corrupt));
            corrupt[byte] ^= (uint8_t)(1U << bit);
            assert(!jetkvm_upload_identity_decode(
                corrupt, sizeof(corrupt), &decoded_size, decoded_digest));
        }
    }
    memcpy(corrupt, record, sizeof(corrupt));
    corrupt[0] ^= 1U;
    assert(!jetkvm_upload_identity_decode(corrupt, sizeof(corrupt),
                                          &decoded_size, decoded_digest));
    memcpy(corrupt, record, sizeof(corrupt));
    corrupt[8] = 2U;
    assert(!jetkvm_upload_identity_decode(corrupt, sizeof(corrupt),
                                          &decoded_size, decoded_digest));
    memcpy(corrupt, record, sizeof(corrupt));
    corrupt[12] = 1U;
    assert(!jetkvm_upload_identity_decode(corrupt, sizeof(corrupt),
                                          &decoded_size, decoded_digest));
    memcpy(corrupt, record, sizeof(corrupt));
    corrupt[24] = 'G';
    assert(!jetkvm_upload_identity_decode(corrupt, sizeof(corrupt),
                                          &decoded_size, decoded_digest));

    memset(record, 0, sizeof(record));
    assert(!jetkvm_upload_identity_encode(0U, LOWER_DIGEST, record));
    assert(!jetkvm_upload_identity_encode(1U, non_hex, record));

    puts("virtual-media upload identity record: PASS");
    return 0;
}
