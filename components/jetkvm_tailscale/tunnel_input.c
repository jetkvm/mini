/* The address of the Tailscale tunnel is reachable through the tunnel only.
 *
 * lwIP takes a packet for an address of the device on each interface, so a
 * host on the local network can send to the tunnel's address through the
 * device's local interface. While a peer has a WireGuard session, lwIP
 * routes the answer by its source address to the tunnel, where the host does
 * not get it. While no peer has one, the tunnel's link is down
 * (wireguardif_periodic()), lwIP routes the answer by its destination, and it
 * leaves through the local interface: the host completes a TCP connection
 * whose local address is the tunnel's address.
 *
 * The web server tells by the local address of a connection that the
 * connection came through Tailscale (jetkvm_access), and the tailnet's packet
 * filter guards only what arrives on the tunnel. Both depend on this rule. */
#include <stdbool.h>
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"

/* The name that wireguardif_init() gives the tunnel. */
static bool is_tunnel(const struct netif *netif)
{
    return netif->name[0] == 'w' && netif->name[1] == 'g';
}

err_t __real_ip4_input(struct pbuf *packet, struct netif *arrival);

/* lwIP's input of each IPv4 packet, on the TCP/IP task (-Wl,--wrap=ip4_input). */
err_t __wrap_ip4_input(struct pbuf *packet, struct netif *arrival)
{
    if (packet->len >= IP_HLEN && !is_tunnel(arrival)) {
        ip4_addr_t destination;
        ip4_addr_copy(destination, ((const struct ip_hdr *)packet->payload)->dest);
        const struct netif *netif;
        NETIF_FOREACH(netif) {
            if (is_tunnel(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif)) &&
                ip4_addr_eq(&destination, netif_ip4_addr(netif))) {
                pbuf_free(packet);
                return ERR_OK;
            }
        }
    }
    return __real_ip4_input(packet, arrival);
}
