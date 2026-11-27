#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Upstream localAuthMaxIdleMinutes: a session timeout of at most a day. */
#define JETKVM_ACCESS_MAX_IDLE_MINUTES 1440U

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

/* "Web interface through Tailscale only". With the setting on, the web server
 * admits a connection only when the address it arrived on is the device's
 * Tailscale IPv4 address. `local` is the 4 bytes of that address, or NULL
 * when the connection did not arrive on an IPv4 address. `tailscale` is the
 * 4 bytes of the device's Tailscale address, or NULL while it has none. The
 * rule names what is admitted, so an address of any other kind is refused. */
static inline bool jetkvm_access_arrived_through_tailscale(const uint8_t *local, const uint8_t *tailscale)
{
    return local && tailscale && memcmp(local, tailscale, 4) == 0;
}
static inline bool jetkvm_access_tailscale_only_admits(bool tailscale_only, const uint8_t *local,
                                                       const uint8_t *tailscale)
{
    return !tailscale_only || jetkvm_access_arrived_through_tailscale(local, tailscale);
}
