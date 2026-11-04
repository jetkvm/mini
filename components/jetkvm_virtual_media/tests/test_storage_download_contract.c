#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "storage_download_contract.h"

#define ISO_SIZE 92807168ULL

static void test_fresh_download(void)
{
    uint64_t total = 1U;
    uint64_t skip = 1U;
    assert(jetkvm_download_check_response(200, (int64_t)ISO_SIZE, "", 0U,
                                          &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_OK);
    assert(total == ISO_SIZE && skip == 0U);

    /* Not found, server error, an unfollowed redirect. */
    assert(jetkvm_download_check_response(404, 0, "", 0U, &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_HTTP_ERROR);
    assert(total == 0U);
    assert(jetkvm_download_check_response(503, 19, "", 0U, &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_HTTP_ERROR);
    assert(jetkvm_download_check_response(302, 0, "", 0U, &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_HTTP_ERROR);
    /* A chunked or empty body cannot be preallocated on the card. */
    assert(jetkvm_download_check_response(200, -1, "", 0U, &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_NO_SIZE);
    assert(jetkvm_download_check_response(200, 0, "", 0U, &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_NO_SIZE);
    assert(jetkvm_download_check_response(204, 0, "", 0U, &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_NO_SIZE);
}

static void test_resumed_download(void)
{
    uint64_t total = 0U;
    uint64_t skip = 0U;
    /* The server continues at the committed prefix. */
    assert(jetkvm_download_check_response(
               206, (int64_t)(ISO_SIZE - 4194304U),
               "bytes 4194304-92807167/92807168", 4194304U, &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_OK);
    assert(total == ISO_SIZE && skip == 0U);

    /* A server that ignores Range sends the whole object: drop the prefix. */
    assert(jetkvm_download_check_response(200, (int64_t)ISO_SIZE, "",
                                          4194304U, &total, &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_OK);
    assert(total == ISO_SIZE && skip == 4194304U);

    /* A range that starts elsewhere, or stops before the end. */
    assert(jetkvm_download_check_response(
               206, 1024, "bytes 0-1023/92807168", 4194304U, &total,
               &skip) == JETKVM_DOWNLOAD_RESPONSE_BAD_RANGE);
    assert(jetkvm_download_check_response(
               206, 1024, "bytes 4194304-4195327/92807168", 4194304U, &total,
               &skip) == JETKVM_DOWNLOAD_RESPONSE_BAD_RANGE);
    /* An unknown total cannot confirm the object is the same one. */
    assert(jetkvm_download_check_response(
               206, 1024, "bytes 4194304-4195327/*", 4194304U, &total,
               &skip) == JETKVM_DOWNLOAD_RESPONSE_NO_SIZE);
    /* An object now shorter than the prefix on the card. */
    assert(jetkvm_download_check_response(200, 1024, "", 4194304U, &total,
                                          &skip) ==
           JETKVM_DOWNLOAD_RESPONSE_BAD_RANGE);
}

static void test_progress_cadence(void)
{
    const int64_t interval = 500000;
    /* The first chunk and the last byte always report. */
    assert(jetkvm_download_progress_due(0U, 0, 65536U, 1000, ISO_SIZE,
                                        interval));
    assert(jetkvm_download_progress_due(ISO_SIZE - 65536U, 1000, ISO_SIZE,
                                        2000, ISO_SIZE, interval));
    /* Within one percent and the interval: no update. */
    assert(!jetkvm_download_progress_due(65536U, 1000, 131072U, 2000,
                                         ISO_SIZE, interval));
    /* A new whole percent reports at once. */
    assert(jetkvm_download_progress_due(ISO_SIZE / 100U - 1U, 1000,
                                        ISO_SIZE / 100U + 1U, 2000, ISO_SIZE,
                                        interval));
    /* A slow link still reports every interval. */
    assert(jetkvm_download_progress_due(65536U, 1000, 131072U,
                                        1000 + interval, ISO_SIZE, interval));
}

int main(void)
{
    test_fresh_download();
    test_resumed_download();
    test_progress_cadence();
    puts("storage download contract: PASS");
    return 0;
}
