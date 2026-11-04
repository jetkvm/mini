#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool jetkvm_access_has_encoded_nul(const char *json)
{
    /* Skip each escaped byte as a unit; \\u0000 is a literal backslash
     * followed by text, while \u0000 decodes to an unrepresentable C string. */
    for (const char *p = json; *p; ++p) {
        if (*p != '\\') continue;
        ++p;
        if (!*p) break;
        if (*p == 'u' && strncmp(p + 1, "0000", 4) == 0) return true;
    }
    return false;
}

static inline uint32_t jetkvm_access_lifetime_ticks(uint32_t seconds, uint32_t hz)
{
    return (uint32_t)((uint64_t)seconds * hz);
}
