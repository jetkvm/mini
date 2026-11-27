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

/* Tailscale gives each node an IPv4 address from 100.64.0.0/10, and the
 * device's tunnel interface has that network. */
static inline bool jetkvm_access_tailscale_range(const uint8_t *ipv4)
{
    return ipv4[0] == 100U && (ipv4[1] & 0xc0U) == 64U;
}
/* "Web interface through Tailscale only". With the setting on, the web server
 * admits a connection only when it arrived on the device's Tailscale IPv4
 * address from a Tailscale address. `local` and `peer` are the 4 bytes of
 * the connection's two addresses, or NULL for one that is not IPv4.
 * `tailscale` is the 4 bytes of the device's Tailscale address, or NULL
 * while it has none. The rule names what is admitted, so an address of any
 * other kind is refused.
 *
 * The peer address is part of the rule because the network stack takes a
 * packet for the Tailscale address on each interface. It sends the answer
 * of such a connection through the tunnel, by the source address, where only
 * a tailnet peer gets it, so a host on the local network does not complete a
 * connection to the Tailscale address. The rule does not depend on that: a
 * peer with an address of the local network is refused by its address. */
static inline bool jetkvm_access_arrived_through_tailscale(const uint8_t *local, const uint8_t *peer,
                                                           const uint8_t *tailscale)
{
    return local && peer && tailscale && memcmp(local, tailscale, 4) == 0 &&
           jetkvm_access_tailscale_range(peer);
}
static inline bool jetkvm_access_tailscale_only_admits(bool tailscale_only, const uint8_t *local,
                                                       const uint8_t *peer, const uint8_t *tailscale)
{
    return !tailscale_only || jetkvm_access_arrived_through_tailscale(local, peer, tailscale);
}
