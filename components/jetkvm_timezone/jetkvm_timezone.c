/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_timezone.h"

#include <string.h>

#include "jetkvm_timezone_table.h"

#define TIMEZONE_COUNT (sizeof(s_timezones) / sizeof(s_timezones[0]))

const char *jetkvm_timezone_name(size_t index)
{
    return index < TIMEZONE_COUNT ? s_timezones[index].name : NULL;
}

const char *jetkvm_timezone_rule(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    size_t low = 0, high = TIMEZONE_COUNT;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        int order = strcmp(name, s_timezones[middle].name);
        if (order == 0) {
            return s_timezones[middle].rule;
        }
        if (order < 0) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return NULL;
}

static bool digit(char value)
{
    return value >= '0' && value <= '9';
}

static bool letter(char value)
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

/* A zone abbreviation: three or more letters, or <...> around letters,
 * digits and signs. */
static bool skip_name(const char **cursor)
{
    const char *text = *cursor;
    size_t length = 0;
    if (*text == '<') {
        for (++text; letter(*text) || digit(*text) || *text == '+' || *text == '-'; ++text) {
            ++length;
        }
        if (*text++ != '>') {
            return false;
        }
    } else {
        for (; letter(*text); ++text) {
            ++length;
        }
    }
    *cursor = text;
    return length >= 3U;
}

static bool number(const char **cursor, int32_t limit, int32_t *value)
{
    const char *text = *cursor;
    if (!digit(*text)) {
        return false;
    }
    int32_t result = 0;
    for (; digit(*text); ++text) {
        result = result * 10 + (*text - '0');
        if (result > limit) {
            return false;
        }
    }
    *cursor = text;
    *value = result;
    return true;
}

/* [+-]hh[:mm[:ss]] in seconds. A change time can be below 0 or above 24 h. */
static bool seconds(const char **cursor, int32_t *value)
{
    const char *text = *cursor;
    bool negative = *text == '-';
    if (*text == '+' || *text == '-') {
        ++text;
    }
    int32_t hours = 0, minutes = 0, second = 0;
    if (!number(&text, 167, &hours)) {
        return false;
    }
    if (*text == ':') {
        ++text;
        if (!number(&text, 59, &minutes)) {
            return false;
        }
        if (*text == ':') {
            ++text;
            if (!number(&text, 59, &second)) {
                return false;
            }
        }
    }
    int32_t total = hours * 3600 + minutes * 60 + second;
    *cursor = text;
    *value = negative ? -total : total;
    return true;
}

typedef struct {
    int32_t month, week, weekday, time;
} change_t;

/* Mm.w.d[/time]: weekday d (0 is Sunday) of week w of month m, where week 5
 * is the last one; the time is 02:00:00 when it is left out. */
static bool change(const char **cursor, change_t *result)
{
    const char *text = *cursor;
    if (*text++ != 'M' || !number(&text, 12, &result->month) || result->month < 1 ||
        *text++ != '.' || !number(&text, 5, &result->week) || result->week < 1 ||
        *text++ != '.' || !number(&text, 6, &result->weekday)) {
        return false;
    }
    result->time = 2 * 3600;
    if (*text == '/') {
        ++text;
        if (!seconds(&text, &result->time)) {
            return false;
        }
    }
    *cursor = text;
    return true;
}

static bool leap(int64_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/* Days from 1970-01-01 to a date of the Gregorian calendar. */
static int64_t days_to(int64_t year, int32_t month, int32_t day)
{
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t year_of_era = year - era * 400;
    int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static int64_t year_of(int64_t days)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t day_of_era = days - era * 146097;
    int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    /* The count starts in March; January and February are of the next year. */
    return year_of_era + era * 400 + (day_of_year >= 306);
}

/* Local seconds from 1970, without an offset, of a change in a year. */
static int64_t change_seconds(const change_t *rule, int64_t year)
{
    static const int8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int64_t first = days_to(year, rule->month, 1);
    /* 1970-01-01 was a Thursday. */
    int32_t first_weekday = (int32_t)(((first + 4) % 7 + 7) % 7);
    int32_t day = 1 + (rule->weekday - first_weekday + 7) % 7 + (rule->week - 1) * 7;
    int32_t last = days_in_month[rule->month - 1] + (rule->month == 2 && leap(year));
    if (day > last) {
        day -= 7;
    }
    return (first + day - 1) * 86400 + rule->time;
}

bool jetkvm_timezone_offset(const char *rule, int64_t utc_seconds, int32_t *offset_seconds)
{
    if (rule == NULL || offset_seconds == NULL) {
        return false;
    }
    /* POSIX counts an offset to the west: local time is UTC minus it. */
    int32_t standard = 0;
    if (!skip_name(&rule) || !seconds(&rule, &standard)) {
        return false;
    }
    if (*rule == '\0') {
        *offset_seconds = -standard;
        return true;
    }
    int32_t daylight = standard - 3600;
    change_t start, end;
    if (!skip_name(&rule) || (*rule != ',' && !seconds(&rule, &daylight)) ||
        *rule++ != ',' || !change(&rule, &start) ||
        *rule++ != ',' || !change(&rule, &end) || *rule != '\0') {
        return false;
    }
    /* A change to daylight time is given in standard time and the change
     * back in daylight time. South of the equator the year starts in
     * daylight time, so the change back comes first. */
    int64_t local = utc_seconds - standard;
    int64_t year = year_of((local >= 0 ? local : local - 86399) / 86400);
    int64_t starts = change_seconds(&start, year) + standard;
    int64_t ends = change_seconds(&end, year) + daylight;
    bool in_daylight = starts < ends ? utc_seconds >= starts && utc_seconds < ends
                                     : utc_seconds >= starts || utc_seconds < ends;
    *offset_seconds = in_daylight ? -daylight : -standard;
    return true;
}

bool jetkvm_timezone_minute_of_day(const char *rule, int64_t utc_seconds, uint16_t *minute)
{
    int32_t offset = 0;
    if (minute == NULL || !jetkvm_timezone_offset(rule, utc_seconds, &offset)) {
        return false;
    }
    int64_t second_of_day = ((utc_seconds + offset) % 86400 + 86400) % 86400;
    *minute = (uint16_t)(second_of_day / 60);
    return true;
}
