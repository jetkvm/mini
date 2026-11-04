/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "web_app_pack.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    FIXTURE_CAPACITY = 512,
    FIXTURE_PARTITION_SIZE = 1024,
    FIXTURE_DATA_OFFSET = 256,
    FIXTURE_PACK_SIZE = 272,
};

static void seal_pack(uint8_t *bytes)
{
    web_app_pack_header_t *header = (web_app_pack_header_t *)bytes;
    header->manifest_crc32 = web_app_crc32(
        bytes + header->entries_offset,
        (size_t)header->entries_size + header->strings_size);
    header->header_crc32 = 0;
    header->header_crc32 = web_app_crc32(header, sizeof(*header));
}

static size_t make_fixture(uint8_t *bytes)
{
    memset(bytes, 0, FIXTURE_CAPACITY);
    web_app_pack_header_t *header = (web_app_pack_header_t *)bytes;
    memcpy(header->magic, WEB_APP_PACK_MAGIC, sizeof(header->magic));
    header->version = WEB_APP_PACK_VERSION;
    header->header_size = sizeof(*header);
    header->entry_count = 2;
    header->entries_offset = sizeof(*header);
    header->entries_size = 2 * sizeof(web_app_pack_entry_t);
    header->strings_offset = header->entries_offset + header->entries_size;
    header->data_offset = FIXTURE_DATA_OFFSET;
    header->data_size = FIXTURE_PACK_SIZE - FIXTURE_DATA_OFFSET;
    header->pack_size = FIXTURE_PACK_SIZE;

    web_app_pack_entry_t *entries =
        (web_app_pack_entry_t *)(bytes + header->entries_offset);
    size_t cursor = header->strings_offset;
    const char *paths[] = { "/static/a.js", "/static/index.html" };
    const char *etags[] = { "\"sha256-a\"", "\"sha256-b\"" };
    static const uint8_t payloads[][8] = {
        { 'p', 'a', 'y', 'l', 'o', 'a', 'd', '!' },
        { 's', 'e', 'c', 'o', 'n', 'd', '!', '!' },
    };
    for (size_t i = 0; i < 2; ++i) {
        size_t path_length = strlen(paths[i]);
        size_t etag_length = strlen(etags[i]);
        entries[i].path_offset = cursor;
        entries[i].path_length = path_length;
        memcpy(bytes + cursor, paths[i], path_length + 1);
        cursor += path_length + 1;
        entries[i].etag_offset = cursor;
        entries[i].etag_length = etag_length;
        memcpy(bytes + cursor, etags[i], etag_length + 1);
        cursor += etag_length + 1;
        entries[i].data_offset = FIXTURE_DATA_OFFSET + i * 8;
        entries[i].stored_size = 8;
        entries[i].source_size = 16;
        memcpy(bytes + entries[i].data_offset, payloads[i],
               sizeof(payloads[i]));
        entries[i].content_crc32 = web_app_crc32(
            bytes + entries[i].data_offset, entries[i].stored_size);
        entries[i].mime = i == 0 ? WEB_APP_MIME_JAVASCRIPT
                                  : WEB_APP_MIME_HTML;
        entries[i].encoding = WEB_APP_ENCODING_GZIP;
        entries[i].cache = i == 0 ? WEB_APP_CACHE_IMMUTABLE
                                  : WEB_APP_CACHE_NO_CACHE;
    }
    header->strings_size = cursor - header->strings_offset;
    seal_pack(bytes);
    return cursor;
}

static void test_valid_pack_and_lookup(void)
{
    uint8_t bytes[FIXTURE_CAPACITY];
    size_t metadata_size = make_fixture(bytes);
    web_app_pack_view_t view;
    assert(web_app_pack_validate(bytes, metadata_size,
                                 FIXTURE_PARTITION_SIZE,
                                 &view) == WEB_APP_PACK_OK);
    const char path[] = "/static/index.html";
    const web_app_pack_entry_t *entry = web_app_pack_find(
        &view, path, sizeof(path) - 1);
    assert(entry != NULL);
    assert(entry->mime == WEB_APP_MIME_HTML);
    assert(strcmp(web_app_pack_entry_etag(&view, entry),
                  "\"sha256-b\"") == 0);
    assert(web_app_pack_find(&view, "/static/missing.js",
                             sizeof("/static/missing.js") - 1) == NULL);
}

static void test_manifest_and_entry_bounds(void)
{
    uint8_t bytes[FIXTURE_CAPACITY];
    size_t metadata_size = make_fixture(bytes);
    web_app_pack_view_t view;

    bytes[sizeof(web_app_pack_header_t)] ^= 1;
    assert(web_app_pack_validate(bytes, metadata_size,
                                 FIXTURE_PARTITION_SIZE, &view) ==
           WEB_APP_PACK_ERR_MANIFEST_CRC);

    metadata_size = make_fixture(bytes);
    web_app_pack_header_t *header = (web_app_pack_header_t *)bytes;
    web_app_pack_entry_t *entries =
        (web_app_pack_entry_t *)(bytes + header->entries_offset);
    entries[0].data_offset = header->pack_size - 3;
    entries[0].stored_size = 8;
    seal_pack(bytes);
    assert(web_app_pack_validate(bytes, metadata_size,
                                 FIXTURE_PARTITION_SIZE, &view) ==
           WEB_APP_PACK_ERR_ENTRY);

    metadata_size = make_fixture(bytes);
    header = (web_app_pack_header_t *)bytes;
    entries = (web_app_pack_entry_t *)(bytes + header->entries_offset);
    web_app_pack_entry_t swap = entries[0];
    entries[0] = entries[1];
    entries[1] = swap;
    seal_pack(bytes);
    assert(web_app_pack_validate(bytes, metadata_size,
                                 FIXTURE_PARTITION_SIZE, &view) ==
           WEB_APP_PACK_ERR_UNSORTED);

    make_fixture(bytes);
    header = (web_app_pack_header_t *)bytes;
    header->pack_size = FIXTURE_PARTITION_SIZE + 1;
    header->data_size = header->pack_size - header->data_offset;
    seal_pack(bytes);
    assert(web_app_pack_header_validate(header, FIXTURE_PARTITION_SIZE,
                                        NULL) == WEB_APP_PACK_ERR_LAYOUT);
}

static void test_distinct_urls_can_share_payload(void)
{
    uint8_t bytes[FIXTURE_CAPACITY];
    size_t metadata_size = make_fixture(bytes);
    web_app_pack_header_t *header = (web_app_pack_header_t *)bytes;
    web_app_pack_entry_t *entries =
        (web_app_pack_entry_t *)(bytes + header->entries_offset);
    entries[1].data_offset = entries[0].data_offset;
    entries[1].stored_size = entries[0].stored_size;
    entries[1].content_crc32 = entries[0].content_crc32;
    seal_pack(bytes);

    web_app_pack_view_t view;
    assert(web_app_pack_validate(bytes, metadata_size,
                                 FIXTURE_PARTITION_SIZE,
                                 &view) == WEB_APP_PACK_OK);
    const web_app_pack_entry_t *script = web_app_pack_find(
        &view, "/static/a.js", sizeof("/static/a.js") - 1);
    const web_app_pack_entry_t *index = web_app_pack_find(
        &view, "/static/index.html", sizeof("/static/index.html") - 1);
    assert(script != NULL && index != NULL);
    assert(script != index);
    assert(script->data_offset == index->data_offset);
    assert(script->stored_size == index->stored_size);
    assert(script->content_crc32 == index->content_crc32);
    assert(memcmp(bytes + script->data_offset, bytes + index->data_offset,
                  script->stored_size) == 0);
    assert(web_app_crc32(bytes + script->data_offset, script->stored_size) ==
           script->content_crc32);
    assert(web_app_crc32(bytes + index->data_offset, index->stored_size) ==
           index->content_crc32);
}

static void expect_range(const char *header, size_t size, size_t first,
                         size_t length)
{
    web_app_byte_range_t range;
    assert(web_app_parse_http_range(header, size, &range) ==
           WEB_APP_RANGE_VALID);
    assert(range.first == first);
    assert(range.length == length);
}

static void test_http_ranges(void)
{
    web_app_byte_range_t range;
    assert(web_app_parse_http_range(NULL, 100, &range) == WEB_APP_RANGE_NONE);
    expect_range("bytes=0-9", 100, 0, 10);
    expect_range("bytes=90-200", 100, 90, 10);
    expect_range("bytes=20-", 100, 20, 80);
    expect_range("bytes=-10", 100, 90, 10);
    expect_range("bytes=-200", 100, 0, 100);
    assert(web_app_parse_http_range("bytes=100-", 100, &range) ==
           WEB_APP_RANGE_INVALID);
    assert(web_app_parse_http_range("bytes=9-3", 100, &range) ==
           WEB_APP_RANGE_INVALID);
    assert(web_app_parse_http_range("bytes=0-1,4-5", 100, &range) ==
           WEB_APP_RANGE_INVALID);
    assert(web_app_parse_http_range("items=0-1", 100, &range) ==
           WEB_APP_RANGE_INVALID);
    assert(web_app_parse_http_range("bytes=0-0", 0, &range) ==
           WEB_APP_RANGE_INVALID);
}

static void test_response_header_tables(void)
{
    assert(strcmp(web_app_content_type(WEB_APP_MIME_JAVASCRIPT),
                  "text/javascript; charset=utf-8") == 0);
    assert(strcmp(web_app_content_type(255), "application/octet-stream") == 0);
    assert(strcmp(web_app_content_encoding(WEB_APP_ENCODING_GZIP), "gzip") == 0);
    assert(web_app_content_encoding(WEB_APP_ENCODING_IDENTITY) == NULL);
    assert(strcmp(web_app_cache_control(WEB_APP_CACHE_IMMUTABLE),
                  "public, max-age=31536000, immutable") == 0);
    assert(strcmp(web_app_cache_control(WEB_APP_CACHE_SHORT),
                  "public, max-age=300") == 0);
    assert(strcmp(web_app_cache_control(WEB_APP_CACHE_NO_CACHE),
                  "no-cache") == 0);
}

int main(void)
{
    test_valid_pack_and_lookup();
    test_manifest_and_entry_bounds();
    test_distinct_urls_can_share_payload();
    test_http_ranges();
    test_response_header_tables();
    puts("web app pack core: OK");
    return 0;
}
