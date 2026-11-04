#ifndef ESP_PEER_INTERNAL_ICE_H
#define ESP_PEER_INTERNAL_ICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "peer_types.h"

typedef enum {
    ICE_CANDIDATE_TYPE_NONE,
    ICE_CANDIDATE_TYPE_HOST,
    ICE_CANDIDATE_TYPE_SRFLX,
    ICE_CANDIDATE_TYPE_PRFLX,
    ICE_CANDIDATE_TYPE_RELAY,
} ice_candidate_type_t;

typedef enum {
    ICE_CANDIDATE_TRANSPORT_UDP,
    ICE_CANDIDATE_TRANSPORT_TCP,
} ice_candidate_transport_t;

typedef struct {
    int foundation;
    int component;
    uint32_t priority;
    ice_candidate_transport_t transport;
    ice_candidate_type_t type;
    esp_peer_addr_t addr;
    esp_peer_addr_t raddr;
} ice_candidate_t;

_Static_assert(sizeof(esp_peer_addr_t) == 20, "vendor address size");
_Static_assert(offsetof(esp_peer_addr_t, family) == 0, "vendor address family offset");
_Static_assert(offsetof(esp_peer_addr_t, port) == 2, "vendor address port offset");
_Static_assert(offsetof(esp_peer_addr_t, ipv6) == 4, "vendor address union offset");
_Static_assert(sizeof(ice_candidate_t) == 60, "vendor candidate size");
_Static_assert(offsetof(ice_candidate_t, foundation) == 0, "vendor foundation offset");
_Static_assert(offsetof(ice_candidate_t, component) == 4, "vendor component offset");
_Static_assert(offsetof(ice_candidate_t, priority) == 8, "vendor priority offset");
_Static_assert(offsetof(ice_candidate_t, transport) == 12, "vendor transport offset");
_Static_assert(offsetof(ice_candidate_t, type) == 16, "vendor candidate type offset");
_Static_assert(offsetof(ice_candidate_t, addr) == 20, "vendor candidate address offset");
_Static_assert(offsetof(ice_candidate_t, raddr) == 40, "vendor related address offset");

void ice_candidate_create(ice_candidate_t *candidate, int foundation,
                          ice_candidate_type_t type, esp_peer_addr_t *addr);
void ice_candidate_create_with_transport(ice_candidate_t *candidate, int foundation,
                                         ice_candidate_type_t type,
                                         ice_candidate_transport_t transport,
                                         esp_peer_addr_t *addr);
void ice_candidate_to_description(ice_candidate_t *candidate, char *description, int length);
int ice_candidate_from_description(ice_candidate_t *candidate, char *description, char *end);

bool addr_ipv4_validate(const char *ipv4, size_t len, esp_peer_addr_t *addr);
bool addr_ipv6_validate(const char *ipv6, size_t len, esp_peer_addr_t *addr);

#endif
