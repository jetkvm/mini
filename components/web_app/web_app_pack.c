/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "web_app_pack.h"

#include <stdbool.h>
#include <string.h>

static bool range_within(size_t offset, size_t size, size_t limit)
{
    return offset <= limit && size <= limit - offset;
}

uint32_t web_app_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

web_app_pack_error_t web_app_pack_header_validate(
    const web_app_pack_header_t *header, size_t partition_size,
    size_t *metadata_map_size)
{
    if (!header || partition_size < sizeof(*header)) {
        return WEB_APP_PACK_ERR_TRUNCATED;
    }
    static const uint8_t magic[8] = WEB_APP_PACK_MAGIC;
    if (memcmp(header->magic, magic, sizeof(magic)) != 0) {
        return WEB_APP_PACK_ERR_MAGIC;
    }
    if (header->version != WEB_APP_PACK_VERSION ||
        header->header_size != sizeof(*header)) {
        return WEB_APP_PACK_ERR_VERSION;
    }

    web_app_pack_header_t copy;
    memcpy(&copy, header, sizeof(copy));
    uint32_t expected_header_crc = copy.header_crc32;
    copy.header_crc32 = 0;
    if (web_app_crc32(&copy, sizeof(copy)) != expected_header_crc) {
        return WEB_APP_PACK_ERR_HEADER_CRC;
    }

    if (header->reserved[0] != 0 || header->reserved[1] != 0 ||
        header->entry_count > UINT32_MAX / sizeof(web_app_pack_entry_t) ||
        header->entries_size !=
            header->entry_count * sizeof(web_app_pack_entry_t) ||
        header->entries_offset != sizeof(*header) ||
        !range_within(header->entries_offset, header->entries_size,
                      header->pack_size) ||
        header->strings_offset !=
            header->entries_offset + header->entries_size ||
        !range_within(header->strings_offset, header->strings_size,
                      header->pack_size) ||
        header->data_offset < header->strings_offset + header->strings_size ||
        header->data_offset > header->pack_size ||
        header->data_size != header->pack_size - header->data_offset ||
        header->pack_size > partition_size) {
        return WEB_APP_PACK_ERR_LAYOUT;
    }
    if (metadata_map_size) {
        *metadata_map_size = header->strings_offset + header->strings_size;
    }
    return WEB_APP_PACK_OK;
}

static bool string_ref_valid(const web_app_pack_view_t *view, uint32_t offset,
                             size_t length)
{
    const web_app_pack_header_t *header = view->header;
    size_t strings_end = header->strings_offset + header->strings_size;
    return offset >= header->strings_offset &&
           range_within(offset, length + 1, strings_end) &&
           offset + length < view->mapped_size &&
           view->base[offset + length] == '\0';
}

static int compare_path(const char *left, size_t left_size, const char *right,
                        size_t right_size)
{
    size_t common = left_size < right_size ? left_size : right_size;
    int compared = memcmp(left, right, common);
    if (compared != 0) {
        return compared;
    }
    return left_size < right_size ? -1 : left_size > right_size ? 1 : 0;
}

web_app_pack_error_t web_app_pack_validate(
    const void *metadata, size_t mapped_size, size_t partition_size,
    web_app_pack_view_t *view)
{
    if (!metadata || mapped_size < sizeof(web_app_pack_header_t) || !view) {
        return WEB_APP_PACK_ERR_TRUNCATED;
    }
    const web_app_pack_header_t *header = metadata;
    size_t required_metadata = 0;
    web_app_pack_error_t error = web_app_pack_header_validate(
        header, partition_size, &required_metadata);
    if (error != WEB_APP_PACK_OK) {
        return error;
    }
    if (mapped_size < required_metadata) {
        return WEB_APP_PACK_ERR_TRUNCATED;
    }
    size_t manifest_size = header->entries_size + header->strings_size;
    if (web_app_crc32((const uint8_t *)metadata + header->entries_offset,
                      manifest_size) != header->manifest_crc32) {
        return WEB_APP_PACK_ERR_MANIFEST_CRC;
    }

    web_app_pack_view_t parsed = {
        .base = metadata,
        .mapped_size = mapped_size,
        .header = header,
        .entries = (const web_app_pack_entry_t *)(
            (const uint8_t *)metadata + header->entries_offset),
    };
    const web_app_pack_entry_t *previous = NULL;
    for (uint32_t i = 0; i < header->entry_count; ++i) {
        const web_app_pack_entry_t *entry = &parsed.entries[i];
        if (entry->reserved != 0 || entry->path_length == 0 ||
            entry->etag_length == 0 || entry->mime <= WEB_APP_MIME_INVALID ||
            entry->mime >= WEB_APP_MIME_COUNT ||
            entry->encoding >= WEB_APP_ENCODING_COUNT ||
            entry->cache >= WEB_APP_CACHE_COUNT ||
            !string_ref_valid(&parsed, entry->path_offset,
                              entry->path_length) ||
            !string_ref_valid(&parsed, entry->etag_offset,
                              entry->etag_length) ||
            entry->data_offset < header->data_offset ||
            !range_within(entry->data_offset, entry->stored_size,
                          header->pack_size)) {
            return WEB_APP_PACK_ERR_ENTRY;
        }
        const char *path = web_app_pack_entry_path(&parsed, entry);
        if (entry->path_length < sizeof("/static/") - 1 ||
            memcmp(path, "/static/", sizeof("/static/") - 1) != 0) {
            return WEB_APP_PACK_ERR_ENTRY;
        }
        if (previous) {
            const char *previous_path =
                web_app_pack_entry_path(&parsed, previous);
            if (compare_path(previous_path, previous->path_length, path,
                             entry->path_length) >= 0) {
                return WEB_APP_PACK_ERR_UNSORTED;
            }
        }
        previous = entry;
    }
    *view = parsed;
    return WEB_APP_PACK_OK;
}

const char *web_app_pack_entry_path(const web_app_pack_view_t *view,
                                    const web_app_pack_entry_t *entry)
{
    return (const char *)view->base + entry->path_offset;
}

const char *web_app_pack_entry_etag(const web_app_pack_view_t *view,
                                    const web_app_pack_entry_t *entry)
{
    return (const char *)view->base + entry->etag_offset;
}

const web_app_pack_entry_t *web_app_pack_find(
    const web_app_pack_view_t *view, const char *path, size_t path_length)
{
    if (!view || !view->header || !path) {
        return NULL;
    }
    size_t low = 0;
    size_t high = view->header->entry_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const web_app_pack_entry_t *entry = &view->entries[middle];
        const char *entry_path = web_app_pack_entry_path(view, entry);
        int compared = compare_path(path, path_length, entry_path,
                                    entry->path_length);
        if (compared == 0) {
            return entry;
        }
        if (compared < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return NULL;
}

const char *web_app_content_type(uint8_t mime)
{
    static const char *const types[WEB_APP_MIME_COUNT] = {
        [WEB_APP_MIME_HTML] = "text/html; charset=utf-8",
        [WEB_APP_MIME_CSS] = "text/css; charset=utf-8",
        [WEB_APP_MIME_JAVASCRIPT] = "text/javascript; charset=utf-8",
        [WEB_APP_MIME_JSON] = "application/json; charset=utf-8",
        [WEB_APP_MIME_SVG] = "image/svg+xml",
        [WEB_APP_MIME_WEB_MANIFEST] = "application/manifest+json",
        [WEB_APP_MIME_TEXT] = "text/plain; charset=utf-8",
        [WEB_APP_MIME_WOFF2] = "font/woff2",
        [WEB_APP_MIME_PNG] = "image/png",
        [WEB_APP_MIME_JPEG] = "image/jpeg",
        [WEB_APP_MIME_GIF] = "image/gif",
        [WEB_APP_MIME_WEBP] = "image/webp",
        [WEB_APP_MIME_ICON] = "image/x-icon",
        [WEB_APP_MIME_AVIF] = "image/avif",
        [WEB_APP_MIME_WASM] = "application/wasm",
        [WEB_APP_MIME_OCTET_STREAM] = "application/octet-stream",
        [WEB_APP_MIME_XML] = "application/xml; charset=utf-8",
    };
    return mime < WEB_APP_MIME_COUNT && types[mime] ? types[mime]
                                                     : types[WEB_APP_MIME_OCTET_STREAM];
}

const char *web_app_content_encoding(uint8_t encoding)
{
    return encoding == WEB_APP_ENCODING_GZIP ? "gzip" : NULL;
}

const char *web_app_cache_control(uint8_t cache)
{
    switch (cache) {
    case WEB_APP_CACHE_IMMUTABLE:
        return "public, max-age=31536000, immutable";
    case WEB_APP_CACHE_SHORT:
        return "public, max-age=300";
    default:
        return "no-cache";
    }
}

static bool parse_decimal(const char *begin, const char *end, size_t *value)
{
    if (begin == end) {
        return false;
    }
    size_t parsed = 0;
    for (const char *cursor = begin; cursor < end; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        unsigned digit = (unsigned)(*cursor - '0');
        if (parsed > (SIZE_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

web_app_range_result_t web_app_parse_http_range(
    const char *value, size_t representation_size, web_app_byte_range_t *range)
{
    if (!value || !*value) {
        return WEB_APP_RANGE_NONE;
    }
    if (!range || representation_size == 0 ||
        strncmp(value, "bytes=", sizeof("bytes=") - 1) != 0) {
        return WEB_APP_RANGE_INVALID;
    }
    const char *spec = value + sizeof("bytes=") - 1;
    const char *end = spec + strlen(spec);
    while (end > spec && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    if (spec == end || memchr(spec, ',', (size_t)(end - spec))) {
        return WEB_APP_RANGE_INVALID;
    }
    const char *dash = memchr(spec, '-', (size_t)(end - spec));
    if (!dash || memchr(dash + 1, '-', (size_t)(end - dash - 1))) {
        return WEB_APP_RANGE_INVALID;
    }

    size_t first = 0;
    size_t last = representation_size - 1;
    if (dash == spec) {
        size_t suffix = 0;
        if (!parse_decimal(dash + 1, end, &suffix) || suffix == 0) {
            return WEB_APP_RANGE_INVALID;
        }
        size_t length = suffix < representation_size ? suffix
                                                     : representation_size;
        first = representation_size - length;
    } else {
        if (!parse_decimal(spec, dash, &first) || first >= representation_size) {
            return WEB_APP_RANGE_INVALID;
        }
        if (dash + 1 < end) {
            if (!parse_decimal(dash + 1, end, &last) || last < first) {
                return WEB_APP_RANGE_INVALID;
            }
            if (last >= representation_size) {
                last = representation_size - 1;
            }
        }
    }
    range->first = first;
    range->length = last - first + 1;
    return WEB_APP_RANGE_VALID;
}

const char *web_app_pack_error_name(web_app_pack_error_t error)
{
    static const char *const names[] = {
        [WEB_APP_PACK_OK] = "ok",
        [WEB_APP_PACK_ERR_TRUNCATED] = "truncated metadata",
        [WEB_APP_PACK_ERR_MAGIC] = "bad magic",
        [WEB_APP_PACK_ERR_VERSION] = "unsupported version",
        [WEB_APP_PACK_ERR_HEADER_CRC] = "header checksum mismatch",
        [WEB_APP_PACK_ERR_LAYOUT] = "invalid pack layout",
        [WEB_APP_PACK_ERR_MANIFEST_CRC] = "manifest checksum mismatch",
        [WEB_APP_PACK_ERR_ENTRY] = "invalid manifest entry",
        [WEB_APP_PACK_ERR_UNSORTED] = "manifest paths are not strictly sorted",
    };
    return error >= 0 && (size_t)error < sizeof(names) / sizeof(names[0]) &&
                   names[error]
               ? names[error]
               : "unknown pack error";
}
