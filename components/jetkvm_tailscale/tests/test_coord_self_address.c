/* The node's own tailnet address from the control server's Node.Addresses. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "cJSON.h"

#include COORD_FUNCTIONS

static uint32_t address_of(const char *json)
{
    cJSON *addresses = cJSON_Parse(json);
    assert(addresses);
    uint32_t address = self_ipv4(addresses);
    cJSON_Delete(addresses);
    return address;
}

int main(void)
{
    assert(address_of("[\"fd7a:115c:a1e0::1/128\", \"100.101.102.103/32\"]") == 0x64656667);
    assert(address_of("[\"100.64.0.1/32\"]") == 0x64400001);
    assert(address_of("[\"100.127.255.254/32\"]") == 0x647ffffe);
    /* A control server such as Headscale can hand out any IPv4 range. */
    assert(address_of("[\"10.0.0.5/32\"]") == 0x0a000005);
    assert(address_of("[\"192.168.100.7/32\"]") == 0xc0a86407);
    assert(address_of("[\"fd7a:115c:a1e0::1/128\", \"10.10.0.5/32\"]") == 0x0a0a0005);
    assert(address_of("[\"100.320.1.2/32\", \"100.80.1.2/32\"]") == 0x64500102);
    assert(address_of("[\"100.320.1.2/32\"]") == 0);
    assert(address_of("[]") == 0);
    puts("Tailscale self address: the first IPv4 address: PASS");
}
