#pragma once
#include <stdbool.h>
#include <stdint.h>
/* RFC 3339, including fractional seconds and numeric UTC offsets.
 * Go's zero time (0001-01-01T00:00:00Z) becomes zero: no key expiry. */
bool ml_timestamp_parse(const char *text, int64_t *epoch);
