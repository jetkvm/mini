#include "ml_key_expiry.h"
#include "ml_timestamp.h"
#include <assert.h>
#include <stdio.h>

static bool update(ml_key_expiry_t *state, const char *json, uint64_t received_ms)
{
    cJSON *map = cJSON_Parse(json);
    assert(map);
    bool valid = ml_key_expiry_update(state, map, received_ms);
    cJSON_Delete(map);
    return valid;
}

void test_key_expiry(void)
{
    int64_t epoch;
    assert(ml_timestamp_parse("2030-01-01T00:00:00Z", &epoch) && epoch == INT64_C(1893456000));
    assert(ml_timestamp_parse("2030-01-01T01:30:00.123456789+01:30", &epoch) && epoch == INT64_C(1893456000));
    assert(ml_timestamp_parse("2024-02-29T00:00:00Z", &epoch) && epoch == INT64_C(1709164800));
    assert(ml_timestamp_parse("0001-01-01T00:00:00Z", &epoch) && epoch == 0);
    const char *invalid[] = {"2023-02-29T00:00:00Z", "2030-13-01T00:00:00Z",
        "2030-01-01T24:00:00Z", "2030-01-01T00:00:00+24:00",
        "2030-01-01T00:00:00.Z", "2030-01-01T00:00:00Zjunk"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!ml_timestamp_parse(invalid[i], &epoch));

    ml_key_expiry_t state = {0};
    assert(update(&state, "{\"ControlTime\":\"2030-01-01T00:00:00Z\","
        "\"Node\":{\"KeyExpiry\":\"2030-01-01T00:00:10Z\"}}", 500));
    // A skewed local clock must not override the server's timestamp.
    assert(!ml_key_expiry_expired(&state, 10499, INT64_C(2208988800)));
    assert(ml_key_expiry_expired(&state, 10500, INT64_C(2208988800)));
    assert(update(&state, "{\"ControlTime\":\"2030-01-01T00:00:11Z\",\"Node\":null}", 600));
    assert(ml_key_expiry_expired(&state, 600, 0));
    assert(update(&state, "{\"Node\":{\"KeyExpiry\":\"0001-01-01T00:00:00Z\"}}", 700));
    assert(!ml_key_expiry_expired(&state, 700, 0));
    assert(update(&state, "{\"Node\":{\"Expired\":true}}", 800));
    assert(ml_key_expiry_expired(&state, 800, 0));
    assert(!update(&state, "{\"Node\":{\"KeyExpiry\":\"2030-99-99T00:00:00Z\"}}", 900));
    assert(ml_key_expiry_expired(&state, 900, 0));
    state = (ml_key_expiry_t){0};
    assert(update(&state, "{\"Node\":{\"KeyExpiry\":\"2030-01-01T00:00:10Z\"}}", 0));
    assert(ml_key_expiry_expired(&state, 0, 0));
    assert(!ml_key_expiry_expired(&state, 0, INT64_C(1893456009)));
    assert(ml_key_expiry_expired(&state, 0, INT64_C(1893456010)));
    state = (ml_key_expiry_t){0};
    assert(update(&state, "{\"ControlTime\":\"2030-01-01T00:00:11Z\","
        "\"Node\":{\"ID\":9007199254740993}}", 100));
    assert(update(&state, "{\"PeersChangedPatch\":[{\"NodeID\":9007199254740992,"
        "\"KeyExpiry\":\"2030-01-01T00:00:10Z\"}]}", 200));
    assert(!ml_key_expiry_expired(&state, 200, 0));
    assert(update(&state, "{\"PeersChangedPatch\":[{\"NodeID\":9007199254740993,"
        "\"KeyExpiry\":\"2030-01-01T00:00:10Z\"}]}", 200));
    assert(ml_key_expiry_expired(&state, 200, 0));
    assert(update(&state, "{\"PeersChangedPatch\":[{\"NodeID\":9007199254740993,"
        "\"KeyExpiry\":null}]}", 300));
    assert(ml_key_expiry_expired(&state, 300, 0));
    assert(update(&state, "{\"PeersChangedPatch\":[{\"NodeID\":9007199254740993,"
        "\"KeyExpiry\":\"0001-01-01T00:00:00Z\"}]}", 400));
    assert(!ml_key_expiry_expired(&state, 400, 0));
    assert(!update(&state, "{\"PeersChangedPatch\":[{\"NodeID\":9007199254740993,"
        "\"KeyExpiry\":\"invalid\"}]}", 500));
    assert(ml_key_expiry_expired(&state, 500, 0));
    puts("Tailscale expiry timestamps, server clock skew and monotonic deadlines: PASS");
}
