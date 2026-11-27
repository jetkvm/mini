/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jetkvm_timezone.h"

/* UTC seconds of a date and time, through the C library. */
static int64_t utc(int year, int month, int day, int hour, int minute, int second)
{
    struct tm tm = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                    .tm_hour = hour, .tm_min = minute, .tm_sec = second};
    return (int64_t)timegm(&tm);
}

static uint16_t minute_at(const char *zone, int64_t when)
{
    uint16_t minute = 0;
    assert(jetkvm_timezone_minute_of_day(jetkvm_timezone_rule(zone), when, &minute));
    return minute;
}

static int32_t offset_at(const char *zone, int64_t when)
{
    int32_t offset = 0;
    assert(jetkvm_timezone_offset(jetkvm_timezone_rule(zone), when, &offset));
    return offset;
}

static void test_table(void)
{
    size_t count = 0;
    for (const char *name; (name = jetkvm_timezone_name(count)) != NULL; ++count) {
        assert(strlen(name) < JETKVM_TIMEZONE_NAME_CAPACITY);
        /* The search finds each name, so the table is in the order it needs. */
        const char *rule = jetkvm_timezone_rule(name);
        assert(rule != NULL);
        if (count != 0) {
            assert(strcmp(jetkvm_timezone_name(count - 1), name) < 0);
        }
    }
    assert(count > 300);
    assert(jetkvm_timezone_name(count + 1) == NULL);
    assert(strcmp(jetkvm_timezone_rule("UTC"), "UTC0") == 0);
    assert(jetkvm_timezone_rule("Europe/Atlantis") == NULL);
    assert(jetkvm_timezone_rule("europe/stockholm") == NULL);
    assert(jetkvm_timezone_rule("") == NULL);
    assert(jetkvm_timezone_rule(NULL) == NULL);
}

/* Times that are known from the calendar, not from another implementation. */
static void test_known_times(void)
{
    /* Sweden changes at 01:00 UTC on the last Sunday of March and of October. */
    assert(minute_at("Europe/Stockholm", utc(2026, 3, 29, 0, 59, 59)) == 1 * 60 + 59);
    assert(minute_at("Europe/Stockholm", utc(2026, 3, 29, 1, 0, 0)) == 3 * 60);
    assert(minute_at("Europe/Stockholm", utc(2026, 10, 25, 0, 59, 59)) == 2 * 60 + 59);
    assert(minute_at("Europe/Stockholm", utc(2026, 10, 25, 1, 0, 0)) == 2 * 60);
    assert(offset_at("Europe/Stockholm", utc(2026, 7, 1, 12, 0, 0)) == 7200);
    assert(offset_at("Europe/Stockholm", utc(2026, 12, 31, 23, 30, 0)) == 3600);
    assert(minute_at("Europe/Stockholm", utc(2026, 12, 31, 23, 30, 0)) == 30);

    /* New York changes at 02:00 local time: the second Sunday of March and
     * the first Sunday of November. */
    assert(offset_at("America/New_York", utc(2027, 3, 14, 6, 59, 59)) == -5 * 3600);
    assert(offset_at("America/New_York", utc(2027, 3, 14, 7, 0, 0)) == -4 * 3600);
    assert(offset_at("America/New_York", utc(2027, 11, 7, 5, 59, 59)) == -4 * 3600);
    assert(offset_at("America/New_York", utc(2027, 11, 7, 6, 0, 0)) == -5 * 3600);

    /* Sydney has daylight time over the new year: from the first Sunday of
     * October to the first Sunday of April. */
    assert(offset_at("Australia/Sydney", utc(2027, 1, 1, 0, 0, 0)) == 11 * 3600);
    assert(offset_at("Australia/Sydney", utc(2027, 4, 3, 15, 59, 59)) == 11 * 3600);
    assert(offset_at("Australia/Sydney", utc(2027, 4, 3, 16, 0, 0)) == 10 * 3600);
    assert(offset_at("Australia/Sydney", utc(2027, 10, 2, 15, 59, 59)) == 10 * 3600);
    assert(offset_at("Australia/Sydney", utc(2027, 10, 2, 16, 0, 0)) == 11 * 3600);

    /* No daylight time, and offsets that are not whole hours. */
    assert(offset_at("Asia/Tokyo", utc(2026, 7, 1, 0, 0, 0)) == 9 * 3600);
    assert(offset_at("Asia/Tokyo", utc(2026, 12, 1, 0, 0, 0)) == 9 * 3600);
    assert(minute_at("Asia/Kolkata", utc(2026, 6, 1, 18, 45, 0)) == 15);
    assert(minute_at("Asia/Kathmandu", utc(2026, 6, 1, 0, 0, 0)) == 5 * 60 + 45);
    assert(offset_at("UTC", utc(2026, 6, 1, 0, 0, 0)) == 0);
    /* Before 1970 the local day still has 1440 minutes. */
    assert(minute_at("Asia/Tokyo", -60) == 8 * 60 + 59);
}

static void test_rejected_rules(void)
{
    static const char *const rules[] = {
        "", "0", "UT0", "CET", "CET-1CEST", "CET-1CEST,M3.5.0", "CET-1CEST,M3.5.0,M10.5.0/3,",
        "CET-1CEST,M13.5.0,M10.5.0", "CET-1CEST,M3.0.0,M10.5.0", "CET-1CEST,M3.6.0,M10.5.0",
        "CET-1CEST,M3.5.7,M10.5.0", "CET-1CEST,J60,J300", "CET-1CEST,60,300", "CET-168",
        "CET-1:60", "<+1>-1", "<+01-1", "CET-1 ", ":Europe/Stockholm",
    };
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i) {
        int32_t offset = 12345;
        uint16_t minute = 12345;
        assert(!jetkvm_timezone_offset(rules[i], 0, &offset));
        assert(!jetkvm_timezone_minute_of_day(rules[i], 0, &minute));
        assert(offset == 12345 && minute == 12345);
    }
    int32_t offset;
    assert(!jetkvm_timezone_offset(NULL, 0, &offset));
    assert(!jetkvm_timezone_offset("UTC0", 0, NULL));
    assert(!jetkvm_timezone_minute_of_day("UTC0", 0, NULL));
}

/* Each rule of the table gives the offset that the C library gives for the
 * same rule, at each half hour of eight years. */
static void test_rules_against_c_library(void)
{
    const int64_t begin = utc(2024, 1, 1, 0, 0, 0), end = utc(2032, 1, 1, 0, 0, 0);
    const char *seen[256];
    size_t seen_count = 0;
    for (size_t index = 0; jetkvm_timezone_name(index) != NULL; ++index) {
        const char *rule = jetkvm_timezone_rule(jetkvm_timezone_name(index));
        bool repeated = false;
        for (size_t i = 0; i < seen_count && !repeated; ++i) {
            repeated = strcmp(seen[i], rule) == 0;
        }
        if (repeated) {
            continue;
        }
        assert(seen_count < sizeof(seen) / sizeof(seen[0]));
        seen[seen_count++] = rule;
        assert(setenv("TZ", rule, 1) == 0);
        tzset();
        for (int64_t when = begin; when < end; when += 1800) {
            time_t moment = (time_t)when;
            struct tm local;
            assert(localtime_r(&moment, &local) != NULL);
            int32_t offset = 0;
            assert(jetkvm_timezone_offset(rule, when, &offset));
            if (offset != (int32_t)local.tm_gmtoff) {
                fprintf(stderr, "%s at %lld: %d, the C library gives %ld\n", rule,
                        (long long)when, (int)offset, (long)local.tm_gmtoff);
                abort();
            }
        }
    }
    assert(seen_count > 50);
    printf("time zone rules compared with the C library: %zu\n", seen_count);
}

int main(void)
{
    test_table();
    test_known_times();
    test_rejected_rules();
    test_rules_against_c_library();
    puts("time zone table and rules: OK");
    return 0;
}
