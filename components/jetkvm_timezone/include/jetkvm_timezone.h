/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest zone name with its terminator, as the jiggler's time zone. */
#define JETKVM_TIMEZONE_NAME_CAPACITY 64U

/* Local time of a time zone, with daylight time, from the table that
 * tools/timezones/generate.py writes. Nothing here reads or sets the
 * process's TZ, so the UTC timestamps of the rest of the firmware stay UTC. */

/* The zone names in the order of strcmp(); NULL from the count on. */
const char *jetkvm_timezone_name(size_t index);

/* The POSIX TZ rule of a zone, for example "CET-1CEST,M3.5.0,M10.5.0/3";
 * NULL for a name that is not in the table. */
const char *jetkvm_timezone_rule(const char *name);

/* The seconds that local time is ahead of UTC at a UTC time, by a rule
 * without daylight time or with both changes as Mm.w.d[/time]. False for a
 * rule of another form. */
bool jetkvm_timezone_offset(const char *rule, int64_t utc_seconds, int32_t *offset_seconds);

/* The minute of the local day, 0..1439, at a UTC time. */
bool jetkvm_timezone_minute_of_day(const char *rule, int64_t utc_seconds, uint16_t *minute);

#ifdef __cplusplus
}
#endif
