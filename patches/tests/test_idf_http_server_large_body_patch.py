#!/usr/bin/env python3
"""Exercise the IDF parser's patched length assignment on large Content-Lengths.

Uses the real HTTP parser and server field types with a 32-bit size_t, so a
64-bit host catches the truncation too, without large payloads.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.0.2/esp-idf'))
PATCH = ROOT / 'patches/esp-idf-6.0.2-http-server-large-body.patch'
assert f'apply_idf_patch(patches/{PATCH.name})' in (ROOT / 'CMakeLists.txt').read_text()
with tempfile.TemporaryDirectory() as temp:
    work = Path(temp)
    for name in ('include/esp_http_server.h', 'src/esp_httpd_priv.h',
                 'src/httpd_parse.c', 'src/httpd_txrx.c'):
        relative = Path('components/esp_http_server') / name
        target = work / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        # Pristine checkout makes the regression test independent of whether
        # configure has already applied the project patch to this SDK.
        target.write_bytes(subprocess.check_output(['git', 'show', f'HEAD:{relative}'], cwd=IDF))
    subprocess.run(['git', 'apply', str(PATCH)], cwd=work, check=True)
    server = work / 'components/esp_http_server'
    request_type = re.search(r'(\w+)\s+content_len;', (server / 'include/esp_http_server.h').read_text())[1]
    remaining_type = re.search(r'(\w+)\s+remaining_len;', (server / 'src/esp_httpd_priv.h').read_text())[1]
    assignment = re.search(r'r->content_len = .*?;', (server / 'src/httpd_parse.c').read_text(), re.S)[0]
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "http_parser.h"
#define size_t uint32_t
struct request { REQUEST_TYPE content_len; } request;
struct auxiliary { REMAINING_TYPE remaining_len; } auxiliary;
static int headers(http_parser *parser) {
    struct request *r = &request;
    ASSIGNMENT
    auxiliary.remaining_len = r->content_len;
    return 0;
}
int main(void) {
    const uint64_t lengths[] = {0, 1024, UINT64_C(4294967295),
        UINT64_C(4294967296), UINT64_C(4294967297),
        UINT64_C(4294967296) + 447 * UINT64_C(1048576),
        UINT64_C(8589934591), UINT64_C(8589934592)};
    for (unsigned i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
        http_parser parser;
        http_parser_init(&parser, HTTP_REQUEST);
        http_parser_settings settings = { .on_headers_complete = headers };
        char header[160];
        int n = snprintf(header, sizeof(header),
            "POST /storage/upload HTTP/1.1\r\nHost: p4\r\nContent-Length: %llu\r\n\r\n",
            (unsigned long long)lengths[i]);
        assert(http_parser_execute(&parser, &settings, header, n) == (unsigned)n);
        assert(request.content_len == lengths[i]);
        assert(auxiliary.remaining_len == lengths[i]);
        uint64_t received = 0;
        while (auxiliary.remaining_len) {
            uint32_t chunk = auxiliary.remaining_len < 24576 ? auxiliary.remaining_len : 24576;
            auxiliary.remaining_len -= chunk;
            received += chunk;
        }
        assert(received == lengths[i]);
    }
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);
    http_parser_settings settings = { .on_headers_complete = headers };
    const char *header = "GET / HTTP/1.1\r\nHost: p4\r\n\r\n";
    http_parser_execute(&parser, &settings, header, strlen(header));
    assert(request.content_len == 0);
    puts("HTTP large body parser and receive accounting: PASS");
}
'''.replace('REQUEST_TYPE', request_type).replace('REMAINING_TYPE', remaining_type).replace('ASSIGNMENT', assignment)
    (work / 'test.c').write_text(source)
    parser_dir = IDF / 'components/http_parser'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(parser_dir),
                    str(work / 'test.c'), str(parser_dir / 'http_parser.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
