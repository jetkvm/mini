/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "jetkvm_virtual_media_upload_identity.h"

#define JETKVM_UPLOAD_SHA256_HEX_LENGTH 64U
#define JETKVM_UPLOAD_IDENTITY_RECORD_VERSION 1U
#define JETKVM_UPLOAD_IDENTITY_RECORD_PAYLOAD_SIZE 88U
#define JETKVM_UPLOAD_IDENTITY_RECORD_SIZE 92U

/* The record is deliberately fixed-width and endian-stable:
 *
 *   0..7   magic "JKUPID01"
 *   8..11  little-endian format version
 *  12..15  reserved zero bytes
 *  16..23  little-endian complete-file size
 *  24..87  lowercase complete-file SHA-256 in hexadecimal
 *  88..91  little-endian IEEE CRC-32 over bytes 0..87
 *
 * The CRC protects the sidecar identity fields against accidental corruption;
 * it is not a checksum for the uploaded payload. A malformed, corrupted or
 * unknown record must never authorize resume. */
static const uint8_t jetkvm_upload_identity_magic[8] = {
    'J', 'K', 'U', 'P', 'I', 'D', '0', '1',
};

static inline char jetkvm_upload_ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) :
                                         value;
}

static inline bool jetkvm_upload_filename_has_suffix_ci(
    const char *filename, const char *suffix)
{
    if (filename == NULL || suffix == NULL) {
        return false;
    }
    const size_t filename_length = strlen(filename);
    const size_t suffix_length = strlen(suffix);
    if (filename_length < suffix_length) {
        return false;
    }
    const char *candidate = filename + filename_length - suffix_length;
    for (size_t i = 0U; i < suffix_length; ++i) {
        if (jetkvm_upload_ascii_lower(candidate[i]) !=
            jetkvm_upload_ascii_lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

static inline bool jetkvm_upload_sha256_hex_valid(const char *hex)
{
    if (hex == NULL) {
        return false;
    }
    for (size_t i = 0U; i < JETKVM_UPLOAD_SHA256_HEX_LENGTH; ++i) {
        const char value = hex[i];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f') ||
              (value >= 'A' && value <= 'F'))) {
            return false;
        }
    }
    return hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH] == '\0';
}

static inline char jetkvm_upload_hex_lower(char value)
{
    return value >= 'A' && value <= 'F' ? (char)(value + ('a' - 'A')) :
                                         value;
}

static inline void jetkvm_upload_sha256_hex_normalize(
    const char *hex, char normalized[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U])
{
    for (size_t i = 0U; i < JETKVM_UPLOAD_SHA256_HEX_LENGTH; ++i) {
        normalized[i] = jetkvm_upload_hex_lower(hex[i]);
    }
    normalized[JETKVM_UPLOAD_SHA256_HEX_LENGTH] = '\0';
}

static inline void jetkvm_upload_store_le32(uint8_t *output, uint32_t value)
{
    for (size_t i = 0U; i < 4U; ++i) {
        output[i] = (uint8_t)(value >> (i * 8U));
    }
}

static inline void jetkvm_upload_store_le64(uint8_t *output, uint64_t value)
{
    for (size_t i = 0U; i < 8U; ++i) {
        output[i] = (uint8_t)(value >> (i * 8U));
    }
}

static inline uint32_t jetkvm_upload_load_le32(const uint8_t *input)
{
    uint32_t value = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        value |= (uint32_t)input[i] << (i * 8U);
    }
    return value;
}

static inline uint64_t jetkvm_upload_load_le64(const uint8_t *input)
{
    uint64_t value = 0U;
    for (size_t i = 0U; i < 8U; ++i) {
        value |= (uint64_t)input[i] << (i * 8U);
    }
    return value;
}

static inline uint32_t jetkvm_upload_identity_crc32(const uint8_t *data,
                                                    size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0U; i < size; ++i) {
        crc ^= data[i];
        for (size_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static inline bool jetkvm_upload_identity_encode(
    uint64_t size, const char *sha256_hex,
    uint8_t record[JETKVM_UPLOAD_IDENTITY_RECORD_SIZE])
{
    if (size == 0U || record == NULL ||
        !jetkvm_upload_sha256_hex_valid(sha256_hex)) {
        return false;
    }
    for (size_t i = 0U; i < 8U; ++i) {
        record[i] = jetkvm_upload_identity_magic[i];
    }
    jetkvm_upload_store_le32(record + 8U,
                             JETKVM_UPLOAD_IDENTITY_RECORD_VERSION);
    for (size_t i = 12U; i < 16U; ++i) {
        record[i] = 0U;
    }
    jetkvm_upload_store_le64(record + 16U, size);
    for (size_t i = 0U; i < JETKVM_UPLOAD_SHA256_HEX_LENGTH; ++i) {
        record[24U + i] =
            (uint8_t)jetkvm_upload_hex_lower(sha256_hex[i]);
    }
    jetkvm_upload_store_le32(
        record + JETKVM_UPLOAD_IDENTITY_RECORD_PAYLOAD_SIZE,
        jetkvm_upload_identity_crc32(
            record, JETKVM_UPLOAD_IDENTITY_RECORD_PAYLOAD_SIZE));
    return true;
}

static inline bool jetkvm_upload_identity_decode(
    const uint8_t *record, size_t record_size, uint64_t *size,
    char sha256_hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U])
{
    if (record == NULL || size == NULL || sha256_hex == NULL ||
        record_size != JETKVM_UPLOAD_IDENTITY_RECORD_SIZE) {
        return false;
    }
    const uint32_t stored_crc = jetkvm_upload_load_le32(
        record + JETKVM_UPLOAD_IDENTITY_RECORD_PAYLOAD_SIZE);
    const uint32_t computed_crc = jetkvm_upload_identity_crc32(
        record, JETKVM_UPLOAD_IDENTITY_RECORD_PAYLOAD_SIZE);
    if (stored_crc != computed_crc) {
        return false;
    }
    for (size_t i = 0U; i < 8U; ++i) {
        if (record[i] != jetkvm_upload_identity_magic[i]) {
            return false;
        }
    }
    if (jetkvm_upload_load_le32(record + 8U) !=
            JETKVM_UPLOAD_IDENTITY_RECORD_VERSION ||
        record[12U] != 0U || record[13U] != 0U ||
        record[14U] != 0U || record[15U] != 0U) {
        return false;
    }
    const uint64_t decoded_size = jetkvm_upload_load_le64(record + 16U);
    if (decoded_size == 0U) {
        return false;
    }
    for (size_t i = 0U; i < JETKVM_UPLOAD_SHA256_HEX_LENGTH; ++i) {
        char value = (char)record[24U + i];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
        sha256_hex[i] = value;
    }
    sha256_hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH] = '\0';
    *size = decoded_size;
    return true;
}

static inline bool jetkvm_upload_identity_matches(
    uint64_t expected_size, const char *expected_sha256_hex,
    uint64_t stored_size, const char *stored_sha256_hex)
{
    if (expected_size != stored_size ||
        !jetkvm_upload_sha256_hex_valid(expected_sha256_hex) ||
        !jetkvm_upload_sha256_hex_valid(stored_sha256_hex)) {
        return false;
    }
    for (size_t i = 0U; i < JETKVM_UPLOAD_SHA256_HEX_LENGTH; ++i) {
        if (jetkvm_upload_hex_lower(expected_sha256_hex[i]) !=
            jetkvm_upload_hex_lower(stored_sha256_hex[i])) {
            return false;
        }
    }
    return true;
}

/* A missing client digest intentionally widens only the requested identity.
 * Stored records still have to decode and contain a valid 64-hex value. */
static inline bool jetkvm_upload_resume_identity_matches(
    uint64_t requested_size, const char *requested_sha256_hex,
    uint64_t stored_size, const char *stored_sha256_hex)
{
    if (requested_size != stored_size ||
        !jetkvm_upload_sha256_hex_valid(requested_sha256_hex) ||
        !jetkvm_upload_sha256_hex_valid(stored_sha256_hex)) {
        return false;
    }
    if (strcmp(requested_sha256_hex,
               JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE) == 0) {
        return true;
    }
    return jetkvm_upload_identity_matches(
        requested_size, requested_sha256_hex,
        stored_size, stored_sha256_hex);
}
