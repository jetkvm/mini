/*
 * ICE candidate parsing and priority helpers.
 *
 * Upstream-derived portions retain this notice:
 * MIT License
 * Copyright (c) 2021 sepfy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "ice.h"
#include "peer_platform.h"
#include "lwip/sockets.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint8_t ice_candidate_type_preference(ice_candidate_type_t type)
{
    switch (type) {
        case ICE_CANDIDATE_TYPE_HOST:
            return 126;
        case ICE_CANDIDATE_TYPE_SRFLX:
            return 100;
        case ICE_CANDIDATE_TYPE_RELAY:
            return 0;
        default:
            return 0;
    }
}

static uint16_t ice_candidate_local_preference(ice_candidate_t *candidate)
{
    return candidate->addr.port;
}

static const char *ice_candidate_get_transport_str(ice_candidate_transport_t transport)
{
    return transport == ICE_CANDIDATE_TRANSPORT_UDP ? "UDP" : "TCP";
}

static ice_candidate_transport_t ice_candidate_get_transport(char *transport)
{
    if (strncasecmp(transport, "UDP", 3) == 0) {
        return ICE_CANDIDATE_TRANSPORT_UDP;
    }
    return ICE_CANDIDATE_TRANSPORT_TCP;
}

static ice_candidate_type_t ice_candidate_get_candidate_type(char *type)
{
    if (strncmp(type, "host", 4) == 0) {
        return ICE_CANDIDATE_TYPE_HOST;
    } else if (strncmp(type, "srflx", 5) == 0) {
        return ICE_CANDIDATE_TYPE_SRFLX;
    } else if (strncmp(type, "relay", 5) == 0) {
        return ICE_CANDIDATE_TYPE_RELAY;
    }
    return ICE_CANDIDATE_TYPE_NONE;
}

static void ice_candidate_priority(ice_candidate_t *candidate)
{
    candidate->priority = (1 << 24) * ice_candidate_type_preference(candidate->type)
                          + (1 << 8) * ice_candidate_local_preference(candidate)
                          + (256 - candidate->component);
}

void ice_candidate_create(ice_candidate_t *candidate, int foundation,
                          ice_candidate_type_t type, esp_peer_addr_t *addr)
{
    ice_candidate_create_with_transport(candidate, foundation, type,
                                        ICE_CANDIDATE_TRANSPORT_UDP, addr);
}

void ice_candidate_create_with_transport(ice_candidate_t *candidate, int foundation,
                                         ice_candidate_type_t type,
                                         ice_candidate_transport_t transport,
                                         esp_peer_addr_t *addr)
{
    memcpy(&candidate->addr, addr, sizeof(esp_peer_addr_t));
    candidate->type = type;
    candidate->foundation = foundation;
    candidate->component = 1;
    ice_candidate_priority(candidate);
    candidate->transport = transport;
}

void ice_candidate_to_description(ice_candidate_t *candidate, char *description, int length)
{
    char typ_raddr[128];
    typ_raddr[0] = 0;
    switch (candidate->type) {
        case ICE_CANDIDATE_TYPE_HOST:
            snprintf(typ_raddr, sizeof(typ_raddr), "host");
            break;
        case ICE_CANDIDATE_TYPE_SRFLX:
        case ICE_CANDIDATE_TYPE_RELAY: {
            const char *type_str = candidate->type == ICE_CANDIDATE_TYPE_SRFLX ? "srflx" : "relay";
            if (candidate->addr.family == AF_INET) {
                snprintf(typ_raddr, sizeof(typ_raddr), "%s raddr %d.%d.%d.%d rport %d",
                         type_str,
                         candidate->raddr.ipv4[0], candidate->raddr.ipv4[1],
                         candidate->raddr.ipv4[2], candidate->raddr.ipv4[3],
                         candidate->raddr.port);
            } else if (candidate->addr.family == AF_INET6) {
                char ip_str[INET6_ADDRSTRLEN];
                lwip_inet_ntop(AF_INET6, candidate->raddr.ipv6, ip_str, sizeof(ip_str));
                snprintf(typ_raddr, sizeof(typ_raddr), "%s raddr %s rport %d",
                         type_str, ip_str, candidate->raddr.port);
            }
            break;
        }
        default:
            break;
    }

    char addr_str[INET6_ADDRSTRLEN];
    if (candidate->addr.family == AF_INET) {
        snprintf(addr_str, sizeof(addr_str), "%d.%d.%d.%d",
                 candidate->addr.ipv4[0], candidate->addr.ipv4[1],
                 candidate->addr.ipv4[2], candidate->addr.ipv4[3]);
    } else if (candidate->addr.family == AF_INET6) {
        lwip_inet_ntop(AF_INET6, candidate->addr.ipv6, addr_str, sizeof(addr_str));
    } else {
        addr_str[0] = 0;
    }
    snprintf(description, length, "a=candidate:%d %d %s %" PRIu32 " %s %d typ %s\r\n",
             candidate->foundation, candidate->component,
             ice_candidate_get_transport_str(candidate->transport),
             candidate->priority, addr_str, candidate->addr.port, typ_raddr);
}

int ice_candidate_from_description(ice_candidate_t *candidate, char *description, char *end)
{
    char *split_start = description;
    char *split_end = NULL;
    int index = 0;
    while (1) {
        split_end = strchr(split_start, ' ');
        char *buf = split_start;
        if (split_end) {
            *split_end = 0;
        }
        int ret = 1;
        switch (index) {
            case 0:
                candidate->foundation = atoi(buf);
                break;
            case 1:
                candidate->component = atoi(buf);
                break;
            case 2:
                candidate->transport = ice_candidate_get_transport(buf);
                break;
            case 3:
                candidate->priority = atoi(buf);
                break;
            case 4:
                if (strstr(buf, "local") == NULL) {
                    if (!addr_ipv4_validate(buf, split_end - split_start, &candidate->addr)) {
                        if (!addr_ipv6_validate(buf, split_end - split_start, &candidate->addr)) {
                            ret = ESP_PEER_ERR_BAD_DATA;
                        }
                    }
                }
                break;
            case 5:
                candidate->addr.port = atoi(buf);
                break;
            case 7:
                candidate->type = ice_candidate_get_candidate_type(buf);
                if (candidate->type == ICE_CANDIDATE_TYPE_NONE) {
                    ret = ESP_PEER_ERR_BAD_DATA;
                    break;
                }
                ret = 0;
                break;
            default:
                break;
        }
        if (split_end) {
            *split_end = ' ';
            split_start = split_end + 1;
        }
        if (ret <= 0) {
            return ret;
        }
        index++;
        if (split_end == NULL || split_start >= end) {
            break;
        }
    }
    return 0;
}
