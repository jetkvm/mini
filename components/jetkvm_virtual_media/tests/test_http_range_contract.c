#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "http_range_contract.h"

int main(void)
{
    assert(jetkvm_http_url_is_supported("http://example.test/disk.img"));
    assert(jetkvm_http_url_is_supported("https://example.test/a.iso?x=1"));
    assert(!jetkvm_http_url_is_supported("ftp://example.test/a.iso"));
    assert(!jetkvm_http_url_is_supported("https:///a.iso"));
    assert(!jetkvm_http_url_is_supported("https://example.test/a b.iso"));

    uint64_t first = 0U;
    uint64_t last = 0U;
    uint64_t total = 0U;
    assert(jetkvm_http_parse_content_range(
        "bytes 0-0/4194304", &first, &last, &total));
    assert(first == 0U && last == 0U && total == 4194304U);
    assert(jetkvm_http_parse_content_range(
        "Bytes 4294967296-4294971391/5368709121",
        &first, &last, &total));
    assert(first == 4294967296ULL && last == 4294971391ULL &&
           total == 5368709121ULL);
    assert(!jetkvm_http_parse_content_range(
        "bytes 0-0/*", &first, &last, &total));
    assert(!jetkvm_http_parse_content_range(
        "bytes 9-1/10", &first, &last, &total));
    assert(!jetkvm_http_parse_content_range(
        "bytes 0-1/1", &first, &last, &total));
    assert(!jetkvm_http_parse_content_range(
        "items 0-0/1", &first, &last, &total));
    puts("virtual-media HTTP Range contract: PASS");
    return 0;
}
