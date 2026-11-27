#include "access_contract.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(jetkvm_access_lifetime_ticks(7*24*60*60, 100) == 60480000);
    assert(jetkvm_access_lifetime_ticks(7*24*60*60, 1000) == 604800000);
    assert(jetkvm_access_has_encoded_nul("{\"password\":\"abc\\u0000def\"}"));
    assert(!jetkvm_access_has_encoded_nul("{\"password\":\"abc\\\\u0000def\"}"));
    assert(!jetkvm_access_has_encoded_nul("{\"password\":\"abc\\u005cu0000def\"}"));
    assert(jetkvm_access_has_encoded_nul("{\"password\":\"abc\\\\\\u0000def\"}"));
    assert(!jetkvm_access_has_encoded_nul("{\"password\":\"abc\\u0001def\"}"));
    /* "Web interface through Tailscale only": off admits every connection;
     * on admits only the one that arrived on the device's Tailscale address
     * from an address of the Tailscale range. */
    const uint8_t tailscale[4] = {100, 101, 102, 103}, lan[4] = {192, 0, 2, 10},
                  neighbour[4] = {100, 101, 102, 104}, lan_host[4] = {192, 0, 2, 77};
    assert(jetkvm_access_tailscale_only_admits(false, lan, lan_host, tailscale));
    assert(jetkvm_access_tailscale_only_admits(false, lan, lan_host, NULL));
    assert(jetkvm_access_tailscale_only_admits(false, NULL, NULL, NULL));
    /* From a tailnet peer to the device's Tailscale address. */
    assert(jetkvm_access_tailscale_only_admits(true, tailscale, neighbour, tailscale));
    assert(!jetkvm_access_tailscale_only_admits(true, lan, lan_host, tailscale));
    /* To an address of the Tailscale range that is not the device's own. */
    assert(!jetkvm_access_tailscale_only_admits(true, neighbour, neighbour, tailscale));
    /* From a host of the local network that sent to the Tailscale address:
     * the stack takes such a packet on each interface. */
    assert(!jetkvm_access_tailscale_only_admits(true, tailscale, lan_host, tailscale));
    /* A connection whose address is not IPv4, on either side. */
    assert(!jetkvm_access_tailscale_only_admits(true, NULL, neighbour, tailscale));
    assert(!jetkvm_access_tailscale_only_admits(true, tailscale, NULL, tailscale));
    /* No Tailscale address: nothing is admitted, also not an equal value. */
    assert(!jetkvm_access_tailscale_only_admits(true, lan, lan_host, NULL));
    assert(!jetkvm_access_tailscale_only_admits(true, tailscale, neighbour, NULL));
    assert(jetkvm_access_arrived_through_tailscale(tailscale, neighbour, tailscale));
    assert(!jetkvm_access_arrived_through_tailscale(lan, neighbour, tailscale));
    assert(!jetkvm_access_arrived_through_tailscale(tailscale, lan_host, tailscale));
    assert(!jetkvm_access_arrived_through_tailscale(tailscale, neighbour, NULL));
    /* The range is 100.64.0.0/10. */
    const uint8_t below[4] = {100, 63, 255, 255}, first[4] = {100, 64, 0, 0},
                  last[4] = {100, 127, 255, 255}, above[4] = {100, 128, 0, 0}, other[4] = {101, 64, 0, 1};
    assert(!jetkvm_access_tailscale_range(below) && jetkvm_access_tailscale_range(first) &&
           jetkvm_access_tailscale_range(last) && !jetkvm_access_tailscale_range(above) &&
           !jetkvm_access_tailscale_range(other));
    puts("Password JSON escapes, seven-day token lifetime and the Tailscale-only rule: OK");
}
