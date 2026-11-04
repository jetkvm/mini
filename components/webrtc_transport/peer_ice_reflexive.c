#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_peer_types.h"
#include "esp_log.h"

/* The caller holds the agent receive lock; these helpers do not lock again. */
extern void *jetkvm_ice_find_pair(void *, const esp_peer_addr_t *);
extern void jetkvm_ice_add_remote(void *, char *, char *);
extern void jetkvm_ice_pair_candidates(void *);

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

/* Read PRIORITY only from the authenticated portion of a Binding Request.
 * This is additional validation; the vendor already checked MESSAGE-INTEGRITY
 * with the current agent's local ICE password before reaching this adapter. */
static uint32_t request_priority(const uint8_t *wire, size_t size)
{
    if (size < 20 || read_be16(wire) != 1 ||
        memcmp(wire + 4, "\x21\x12\xa4\x42", 4) != 0 ||
        (size_t)read_be16(wire + 2) + 20U != size) return 0;
    uint32_t priority = 0;
    for (size_t pos = 20; pos + 4 <= size;) {
        uint16_t type = read_be16(wire + pos);
        size_t length = read_be16(wire + pos + 2);
        size_t padded = (length + 3U) & ~(size_t)3U;
        if (padded > size - pos - 4) return 0;
        if (type == 8) return length == 20 ? priority : 0;
        if (type == 0x24) {
            if (length != 4 || priority) return 0;
            const uint8_t *p = wire + pos + 4;
            priority = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8) | p[3];
        }
        pos += 4 + padded;
    }
    return 0;
}

void *jetkvm_ice_authenticated_pair(void *agent, const esp_peer_addr_t *source,
                                  const uint8_t *wire, size_t size)
{
    void *pair = jetkvm_ice_find_pair(agent, source);
    if (pair || !source || !wire || source->family != AF_INET || !source->port)
        return pair;

    if (size < 20 || size > 1400) return NULL;
    uint32_t priority = request_priority(wire, size);
    if (!priority) return NULL;

    char candidate[128];
    int length = snprintf(candidate, sizeof(candidate),
        "%u 1 UDP %lu %u.%u.%u.%u %u typ host",
        (unsigned)source->port, (unsigned long)priority,
        source->ipv4[0], source->ipv4[1], source->ipv4[2], source->ipv4[3],
        (unsigned)source->port);
    if (length <= 0 || (size_t)length >= sizeof(candidate)) return NULL;
    /* This SDK has no prflx type and pairs only equal candidate types. Store
     * the learned direct UDP endpoint as a host candidate internally, with
     * the request's PRIORITY. It is not advertised in SDP. Use the normal
     * bounded allocators, preserving existing candidates and credentials. */
    jetkvm_ice_add_remote(agent, candidate, candidate + length);
    jetkvm_ice_pair_candidates(agent);
    pair = jetkvm_ice_find_pair(agent, source);
    if (pair) ESP_LOGI("webrtc_ice", "authenticated peer-reflexive candidate %u.%u.%u.%u:%u",
        source->ipv4[0], source->ipv4[1], source->ipv4[2], source->ipv4[3],
        (unsigned)source->port);
    return pair;
}
