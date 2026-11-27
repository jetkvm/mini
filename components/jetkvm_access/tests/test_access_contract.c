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
     * on admits only the one that arrived on the device's Tailscale address. */
    const uint8_t tailscale[4] = {100, 101, 102, 103}, lan[4] = {192, 0, 2, 10},
                  neighbour[4] = {100, 101, 102, 104};
    assert(jetkvm_access_tailscale_only_admits(false, lan, tailscale));
    assert(jetkvm_access_tailscale_only_admits(false, lan, NULL));
    assert(jetkvm_access_tailscale_only_admits(false, NULL, NULL));
    assert(jetkvm_access_tailscale_only_admits(true, tailscale, tailscale));
    assert(!jetkvm_access_tailscale_only_admits(true, lan, tailscale));
    assert(!jetkvm_access_tailscale_only_admits(true, neighbour, tailscale));
    /* A connection that did not arrive on an IPv4 address. */
    assert(!jetkvm_access_tailscale_only_admits(true, NULL, tailscale));
    /* No Tailscale address: nothing is admitted, also not an equal value. */
    assert(!jetkvm_access_tailscale_only_admits(true, lan, NULL));
    assert(!jetkvm_access_tailscale_only_admits(true, tailscale, NULL));
    assert(jetkvm_access_arrived_through_tailscale(tailscale, tailscale));
    assert(!jetkvm_access_arrived_through_tailscale(lan, tailscale));
    assert(!jetkvm_access_arrived_through_tailscale(tailscale, NULL));
    puts("Password JSON escapes, seven-day token lifetime and the Tailscale-only rule: OK");
}
