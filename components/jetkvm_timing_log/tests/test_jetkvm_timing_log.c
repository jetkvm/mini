#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jetkvm_timing_log.h"

static void expect_json(const char *expected)
{
    char *json = jetkvm_timing_log_json();
    assert(json != NULL);
    if (strcmp(json, expected) != 0) {
        fprintf(stderr, "got  %s\nwant %s\n", json, expected);
        abort();
    }
    free(json);
}

/* The number of entries in the list that starts at name, and its first and
 * last entry. */
static uint32_t list(const char *json, const char *name, int64_t first[2], int64_t last[2])
{
    const char *at = strstr(json, name);
    assert(at != NULL);
    at += strlen(name);
    assert(*at == '[');
    at++;
    uint32_t entries = 0U;
    while (*at == '[' || *at == ',') {
        at += *at == ',' ? 2 : 1;
        char *end = NULL;
        last[0] = strtoll(at, &end, 10);
        assert(*end == ',');
        last[1] = strtoll(end + 1, &end, 10);
        assert(*end == ']');
        at = end + 1;
        if (entries++ == 0U) {
            first[0] = last[0];
            first[1] = last[1];
        }
    }
    assert(*at == ']');
    return entries;
}

int main(void)
{
    const char *stopped = "{\"running\":false,\"keyCount\":0,\"frameCount\":0,\"keys\":[],\"frames\":[]}";

    /* Without a start, nothing is kept. */
    jetkvm_timing_log_key(4U, 100);
    jetkvm_timing_log_frame(9000U, 200);
    expect_json(stopped);

    assert(jetkvm_timing_log_start() == ESP_OK);
    jetkvm_timing_log_key(4U, 1000);
    jetkvm_timing_log_frame(90000U, 1500);
    jetkvm_timing_log_key(0U, 2000);
    jetkvm_timing_log_frame(91530U, 18166);
    jetkvm_timing_log_frame(93060U, 34833);
    expect_json("{\"running\":true,\"keyCount\":2,\"frameCount\":3,"
                "\"keys\":[[1000,4],[2000,0]],"
                "\"frames\":[[1500,90000],[18166,91530],[34833,93060]]}");

    /* A read does not empty the log, and the log takes entries after it. */
    jetkvm_timing_log_key(5U, 3000);
    expect_json("{\"running\":true,\"keyCount\":3,\"frameCount\":3,"
                "\"keys\":[[1000,4],[2000,0],[3000,5]],"
                "\"frames\":[[1500,90000],[18166,91530],[34833,93060]]}");

    /* A second start empties the log. */
    assert(jetkvm_timing_log_start() == ESP_OK);
    expect_json("{\"running\":true,\"keyCount\":0,\"frameCount\":0,\"keys\":[],\"frames\":[]}");

    /* A full log keeps the newest entries, oldest first. The largest values
     * fit the string: the sanitizer stops the test when they do not. */
    const uint32_t extra = 5U;
    for (uint32_t i = 0U; i < JETKVM_TIMING_LOG_FRAMES + extra; i++) {
        jetkvm_timing_log_frame(UINT32_MAX - i, INT64_MAX - i);
    }
    for (uint32_t i = 0U; i < JETKVM_TIMING_LOG_KEYS + extra; i++) {
        jetkvm_timing_log_key((uint8_t)(i % 251U), INT64_MIN + i);
    }
    char *json = jetkvm_timing_log_json();
    assert(json != NULL);
    char counts[96];
    snprintf(counts, sizeof(counts), "{\"running\":true,\"keyCount\":%" PRIu32 ",\"frameCount\":%" PRIu32 ",",
             JETKVM_TIMING_LOG_KEYS + extra, JETKVM_TIMING_LOG_FRAMES + extra);
    assert(strncmp(json, counts, strlen(counts)) == 0);
    int64_t first[2], last[2];
    assert(list(json, "\"frames\":", first, last) == JETKVM_TIMING_LOG_FRAMES);
    assert(first[0] == INT64_MAX - extra && first[1] == (int64_t)(UINT32_MAX - extra));
    assert(last[0] == INT64_MAX - (JETKVM_TIMING_LOG_FRAMES + extra - 1U));
    assert(list(json, "\"keys\":", first, last) == JETKVM_TIMING_LOG_KEYS);
    assert(first[0] == INT64_MIN + extra && first[1] == (int64_t)(extra % 251U));
    assert(last[1] == (int64_t)((JETKVM_TIMING_LOG_KEYS + extra - 1U) % 251U));
    assert(json[strlen(json) - 1U] == '}');
    free(json);

    jetkvm_timing_log_stop();
    jetkvm_timing_log_key(4U, 100);
    expect_json(stopped);

    puts("jetkvm_timing_log host tests: OK");
    return 0;
}
