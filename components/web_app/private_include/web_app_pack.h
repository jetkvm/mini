/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_APP_PACK_MAGIC "JKWPACK"
#define WEB_APP_PACK_VERSION 1U
#define WEB_APP_PACK_HEADER_SIZE 64U
#define WEB_APP_PACK_ENTRY_SIZE 32U

typedef enum {
    WEB_APP_MIME_INVALID = 0,
    WEB_APP_MIME_HTML = 1,
    WEB_APP_MIME_CSS,
    WEB_APP_MIME_JAVASCRIPT,
    WEB_APP_MIME_JSON,
    WEB_APP_MIME_SVG,
    WEB_APP_MIME_WEB_MANIFEST,
    WEB_APP_MIME_TEXT,
    WEB_APP_MIME_WOFF2,
    WEB_APP_MIME_PNG,
    WEB_APP_MIME_JPEG,
    WEB_APP_MIME_GIF,
    WEB_APP_MIME_WEBP,
    WEB_APP_MIME_ICON,
    WEB_APP_MIME_AVIF,
    WEB_APP_MIME_WASM,
    WEB_APP_MIME_OCTET_STREAM,
    WEB_APP_MIME_XML,
    WEB_APP_MIME_COUNT,
} web_app_mime_t;

typedef enum {
    WEB_APP_ENCODING_IDENTITY = 0,
    WEB_APP_ENCODING_GZIP = 1,
    WEB_APP_ENCODING_COUNT,
} web_app_encoding_t;

typedef enum {
    WEB_APP_CACHE_NO_CACHE = 0,
    WEB_APP_CACHE_IMMUTABLE = 1,
    WEB_APP_CACHE_SHORT = 2,
    WEB_APP_CACHE_COUNT,
} web_app_cache_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t entry_count;
    uint32_t entries_offset;
    uint32_t entries_size;
    uint32_t strings_offset;
    uint32_t strings_size;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t pack_size;
    uint32_t manifest_crc32;
    uint32_t header_crc32;
    uint32_t reserved[2];
} web_app_pack_header_t;

typedef struct __attribute__((packed)) {
    uint32_t path_offset;
    uint32_t data_offset;
    uint32_t stored_size;
    uint32_t source_size;
    uint32_t etag_offset;
    uint32_t content_crc32;
    uint16_t path_length;
    uint8_t etag_length;
    uint8_t mime;
    uint8_t encoding;
    uint8_t cache;
    uint16_t reserved;
} web_app_pack_entry_t;

typedef enum {
    WEB_APP_PACK_OK = 0,
    WEB_APP_PACK_ERR_TRUNCATED,
    WEB_APP_PACK_ERR_MAGIC,
    WEB_APP_PACK_ERR_VERSION,
    WEB_APP_PACK_ERR_HEADER_CRC,
    WEB_APP_PACK_ERR_LAYOUT,
    WEB_APP_PACK_ERR_MANIFEST_CRC,
    WEB_APP_PACK_ERR_ENTRY,
    WEB_APP_PACK_ERR_UNSORTED,
} web_app_pack_error_t;

typedef struct {
    const uint8_t *base;
    size_t mapped_size;
    const web_app_pack_header_t *header;
    const web_app_pack_entry_t *entries;
} web_app_pack_view_t;

typedef struct {
    size_t first;
    size_t length;
} web_app_byte_range_t;

typedef enum {
    WEB_APP_RANGE_NONE = 0,
    WEB_APP_RANGE_VALID,
    WEB_APP_RANGE_INVALID,
} web_app_range_result_t;

_Static_assert(sizeof(web_app_pack_header_t) == WEB_APP_PACK_HEADER_SIZE,
               "web app pack header layout changed");
_Static_assert(sizeof(web_app_pack_entry_t) == WEB_APP_PACK_ENTRY_SIZE,
               "web app pack entry layout changed");

uint32_t web_app_crc32(const void *data, size_t size);

web_app_pack_error_t web_app_pack_header_validate(
    const web_app_pack_header_t *header, size_t partition_size,
    size_t *metadata_map_size);

web_app_pack_error_t web_app_pack_validate(
    const void *metadata, size_t mapped_size, size_t partition_size,
    web_app_pack_view_t *view);

const web_app_pack_entry_t *web_app_pack_find(
    const web_app_pack_view_t *view, const char *path, size_t path_length);

const char *web_app_pack_entry_path(const web_app_pack_view_t *view,
                                    const web_app_pack_entry_t *entry);
const char *web_app_pack_entry_etag(const web_app_pack_view_t *view,
                                    const web_app_pack_entry_t *entry);

const char *web_app_content_type(uint8_t mime);
const char *web_app_content_encoding(uint8_t encoding);
const char *web_app_cache_control(uint8_t cache);

web_app_range_result_t web_app_parse_http_range(
    const char *value, size_t representation_size, web_app_byte_range_t *range);

const char *web_app_pack_error_name(web_app_pack_error_t error);

#ifdef __cplusplus
}
#endif
