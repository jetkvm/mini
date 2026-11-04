/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_hid_profiles.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint64_t seconds;
    uint64_t minutes;
    uint32_t hours;
    uint32_t days_of_month;
    uint16_t months;
    uint8_t days_of_week;
    bool day_of_month_wildcard;
    bool day_of_week_wildcard;
} parsed_cron_t;

static bool terminated(const char *value, size_t capacity, bool allow_empty)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL &&
           (allow_empty || value[0] != '\0');
}

static size_t bounded_length(const char *value, size_t capacity)
{
    if (value == NULL) {
        return capacity;
    }
    size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        length++;
    }
    return length;
}

static bool parse_unsigned(const char *begin, const char *end,
                           unsigned *value)
{
    if (begin == NULL || end == NULL || begin >= end || value == NULL) {
        return false;
    }
    unsigned parsed = 0U;
    for (const char *cursor = begin; cursor < end; ++cursor) {
        if (*cursor < '0' || *cursor > '9' ||
            parsed > (UINT_MAX - (unsigned)(*cursor - '0')) / 10U) {
            return false;
        }
        parsed = parsed * 10U + (unsigned)(*cursor - '0');
    }
    *value = parsed;
    return true;
}

static bool set_cron_range(uint64_t *mask, unsigned minimum,
                           unsigned maximum, unsigned start, unsigned end,
                           unsigned step, bool sunday_is_seven)
{
    if (mask == NULL || step == 0U || start < minimum || end > maximum ||
        start > end) {
        return false;
    }
    for (unsigned value = start; value <= end;) {
        unsigned bit = sunday_is_seven && value == 7U ? 0U : value;
        *mask |= UINT64_C(1) << bit;
        if (end - value < step) {
            break;
        }
        value += step;
    }
    return true;
}

static bool parse_cron_field(const char *text, unsigned minimum,
                             unsigned maximum, bool sunday_is_seven,
                             uint64_t *mask, bool *wildcard)
{
    if (text == NULL || text[0] == '\0' || mask == NULL) {
        return false;
    }
    *mask = 0U;
    if (wildcard != NULL) {
        *wildcard = strcmp(text, "*") == 0;
    }

    const char *item = text;
    while (*item != '\0') {
        const char *item_end = strchr(item, ',');
        if (item_end == NULL) {
            item_end = item + strlen(item);
        }
        if (item == item_end) {
            return false;
        }

        const char *slash = memchr(item, '/', (size_t)(item_end - item));
        const char *range_end = slash != NULL ? slash : item_end;
        unsigned step = 1U;
        if (slash != NULL &&
            (!parse_unsigned(slash + 1, item_end, &step) || step == 0U)) {
            return false;
        }

        unsigned start = minimum;
        unsigned end = maximum;
        if (!(range_end - item == 1 && item[0] == '*')) {
            const char *dash = memchr(item, '-',
                                      (size_t)(range_end - item));
            if (dash != NULL) {
                if (!parse_unsigned(item, dash, &start) ||
                    !parse_unsigned(dash + 1, range_end, &end)) {
                    return false;
                }
            } else {
                if (!parse_unsigned(item, range_end, &start)) {
                    return false;
                }
                end = slash != NULL ? maximum : start;
            }
        }
        if (!set_cron_range(mask, minimum, maximum, start, end, step,
                            sunday_is_seven)) {
            return false;
        }
        item = *item_end == ',' ? item_end + 1 : item_end;
    }
    return *mask != 0U;
}

static bool tokenize_cron(const char *expression,
                          char fields[6][JETKVM_CONTROL_JIGGLER_CRON_CAPACITY])
{
    if (expression == NULL ||
        bounded_length(expression, JETKVM_CONTROL_JIGGLER_CRON_CAPACITY) >=
            JETKVM_CONTROL_JIGGLER_CRON_CAPACITY) {
        return false;
    }
    size_t count = 0U;
    const char *cursor = expression;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count == 6U) {
            return false;
        }
        const char *start = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        size_t length = (size_t)(cursor - start);
        if (length == 0U || length >= sizeof(fields[count])) {
            return false;
        }
        memcpy(fields[count], start, length);
        fields[count][length] = '\0';
        count++;
    }
    return count == 6U;
}

static bool parse_cron(const char *expression, parsed_cron_t *cron)
{
    if (cron == NULL) {
        return false;
    }
    char fields[6][JETKVM_CONTROL_JIGGLER_CRON_CAPACITY] = {{0}};
    memset(cron, 0, sizeof(*cron));
    uint64_t hours = 0U;
    uint64_t days_of_month = 0U;
    uint64_t months = 0U;
    uint64_t days_of_week = 0U;
    if (!tokenize_cron(expression, fields) ||
        !parse_cron_field(fields[0], 0U, 59U, false, &cron->seconds,
                          NULL) ||
        !parse_cron_field(fields[1], 0U, 59U, false, &cron->minutes,
                          NULL) ||
        !parse_cron_field(fields[2], 0U, 23U, false, &hours, NULL) ||
        !parse_cron_field(fields[3], 1U, 31U, false, &days_of_month,
                          &cron->day_of_month_wildcard) ||
        !parse_cron_field(fields[4], 1U, 12U, false, &months, NULL) ||
        !parse_cron_field(fields[5], 0U, 7U, true, &days_of_week,
                          &cron->day_of_week_wildcard)) {
        return false;
    }
    cron->hours = (uint32_t)hours;
    cron->days_of_month = (uint32_t)days_of_month;
    cron->months = (uint16_t)months;
    cron->days_of_week = (uint8_t)days_of_week;
    return true;
}

void jetkvm_hid_profiles_defaults(
    jetkvm_control_keyboard_macro_list_t *macros, bool *jiggler_enabled,
    jetkvm_control_jiggler_config_t *jiggler_config)
{
    if (macros != NULL) {
        memset(macros, 0, sizeof(*macros));
    }
    if (jiggler_enabled != NULL) {
        *jiggler_enabled = false;
    }
    if (jiggler_config != NULL) {
        memset(jiggler_config, 0, sizeof(*jiggler_config));
        jiggler_config->inactivity_limit_seconds = 60U;
        jiggler_config->jitter_percentage = 25U;
        memcpy(jiggler_config->schedule_cron_tab,
               JETKVM_CONTROL_JIGGLER_DEFAULT_CRON,
               sizeof(JETKVM_CONTROL_JIGGLER_DEFAULT_CRON));
        memcpy(jiggler_config->timezone,
               JETKVM_CONTROL_JIGGLER_DEFAULT_TIMEZONE,
               sizeof(JETKVM_CONTROL_JIGGLER_DEFAULT_TIMEZONE));
    }
}

bool jetkvm_hid_profiles_macros_valid(
    const jetkvm_control_keyboard_macro_list_t *macros)
{
    if (macros == NULL ||
        macros->count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_COUNT) {
        return false;
    }
    for (size_t i = 0U; i < macros->count; ++i) {
        const jetkvm_control_keyboard_macro_t *macro = &macros->macros[i];
        if (!terminated(macro->id, sizeof(macro->id), false) ||
            !terminated(macro->name, sizeof(macro->name), false) ||
            macro->step_count == 0U ||
            macro->step_count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_STEPS) {
            return false;
        }
        for (size_t j = 0U; j < macro->step_count; ++j) {
            const jetkvm_control_keyboard_macro_step_t *step =
                &macro->steps[j];
            if (step->key_count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_KEYS ||
                step->modifier_count >
                    JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS ||
                step->delay_ms < 50U || step->delay_ms > 2000U) {
                return false;
            }
            for (size_t k = 0U; k < step->key_count; ++k) {
                if (step->key_ids[k] >=
                    JETKVM_CONTROL_KEYBOARD_MACRO_KEY_NAME_COUNT) {
                    return false;
                }
            }
            for (size_t k = 0U; k < step->modifier_count; ++k) {
                if (step->modifier_ids[k] >=
                    JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool jetkvm_hid_profiles_cron_valid(const char *expression)
{
    parsed_cron_t cron;
    return parse_cron(expression, &cron);
}

bool jetkvm_hid_profiles_cron_matches(const char *expression,
                                      const struct tm *utc)
{
    parsed_cron_t cron;
    if (utc == NULL || utc->tm_sec < 0 || utc->tm_sec > 59 ||
        utc->tm_min < 0 || utc->tm_min > 59 || utc->tm_hour < 0 ||
        utc->tm_hour > 23 || utc->tm_mday < 1 || utc->tm_mday > 31 ||
        utc->tm_mon < 0 || utc->tm_mon > 11 || utc->tm_wday < 0 ||
        utc->tm_wday > 6 || !parse_cron(expression, &cron)) {
        return false;
    }
    bool day_of_month =
        (cron.days_of_month & (UINT32_C(1) << utc->tm_mday)) != 0U;
    bool day_of_week =
        (cron.days_of_week & (uint8_t)(UINT8_C(1) << utc->tm_wday)) != 0U;
    bool day_matches = false;
    if (cron.day_of_month_wildcard && cron.day_of_week_wildcard) {
        day_matches = true;
    } else if (cron.day_of_month_wildcard) {
        day_matches = day_of_week;
    } else if (cron.day_of_week_wildcard) {
        day_matches = day_of_month;
    } else {
        day_matches = day_of_month || day_of_week;
    }
    return (cron.seconds & (UINT64_C(1) << utc->tm_sec)) != 0U &&
           (cron.minutes & (UINT64_C(1) << utc->tm_min)) != 0U &&
           (cron.hours & (UINT32_C(1) << utc->tm_hour)) != 0U &&
           (cron.months &
            (uint16_t)(UINT16_C(1) << (unsigned)(utc->tm_mon + 1))) != 0U &&
           day_matches;
}

bool jetkvm_hid_profiles_jiggler_config_valid(
    const jetkvm_control_jiggler_config_t *config)
{
    return config != NULL && config->inactivity_limit_seconds >= 1U &&
           config->inactivity_limit_seconds <= 86400U &&
           config->jitter_percentage <= 100U &&
           terminated(config->schedule_cron_tab,
                      sizeof(config->schedule_cron_tab), false) &&
           terminated(config->timezone, sizeof(config->timezone), false) &&
           strcmp(config->timezone,
                  JETKVM_CONTROL_JIGGLER_DEFAULT_TIMEZONE) == 0 &&
           jetkvm_hid_profiles_cron_valid(config->schedule_cron_tab);
}
