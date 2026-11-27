/* The address of the tunnel is taken from the tunnel only: the production
 * input rule in front of lwIP's own IPv4 input and UDP delivery, with the
 * tunnel interface that the WireGuard library makes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/udp.h"
#include "lwip/udp.h"
#include "wireguardif.h"

enum { PORT = 80 };
static unsigned delivered;

uint32_t sys_now(void) { return 1000; }
uint32_t wireguard_sys_now(void) { return sys_now(); }
void wireguard_random_bytes(void *data, size_t length) { memset(data, 0x42, length); }
void wireguard_tai64n_now(uint8_t *data) { memset(data, 0, 12); }
bool wireguard_is_under_load(void) { return false; }
err_t __real_ip4_input(struct pbuf *packet, struct netif *arrival);

static void received(void *context, struct udp_pcb *pcb, struct pbuf *packet, const ip_addr_t *source, u16_t port)
{
    (void)context; (void)pcb; (void)source; (void)port;
    ++delivered;
    pbuf_free(packet);
}

/* The stack's answers, such as ICMP, go nowhere. */
static err_t no_output(struct netif *netif, struct pbuf *packet, const ip4_addr_t *destination)
{
    (void)netif; (void)packet; (void)destination;
    return ERR_OK;
}

static err_t interface_ready(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = no_output;
    netif->mtu = 1500;
    return ERR_OK;
}

static void add(struct netif *netif, const char *address, const char *mask, void *state, netif_init_fn ready)
{
    ip4_addr_t own, netmask, gateway = {0};
    assert(ip4addr_aton(address, &own) && ip4addr_aton(mask, &netmask));
    assert(netif_add(netif, &own, &netmask, &gateway, state, ready, ip_input));
    netif_set_up(netif);
}

static void readdress(struct netif *netif, const char *address)
{
    ip4_addr_t own;
    assert(ip4addr_aton(address, &own));
    netif_set_ipaddr(netif, &own);
}

/* `length` bytes of a UDP datagram to PORT; a complete one has 32 bytes. */
static struct pbuf *datagram(const char *source, const char *destination, u16_t length)
{
    uint8_t bytes[IP_HLEN + UDP_HLEN + 4] = {0};
    struct ip_hdr *ip = (struct ip_hdr *)bytes;
    struct udp_hdr *udp = (struct udp_hdr *)(bytes + IP_HLEN);
    ip4_addr_t from, to;
    assert(ip4addr_aton(source, &from) && ip4addr_aton(destination, &to) && length <= sizeof(bytes));
    IPH_VHL_SET(ip, 4, IP_HLEN / 4);
    IPH_LEN_SET(ip, lwip_htons(sizeof(bytes)));
    IPH_TTL_SET(ip, 64);
    IPH_PROTO_SET(ip, IP_PROTO_UDP);
    ip4_addr_copy(ip->src, from);
    ip4_addr_copy(ip->dest, to);
    IPH_CHKSUM_SET(ip, inet_chksum(ip, IP_HLEN));
    udp->src = lwip_htons(40000);
    udp->dest = lwip_htons(PORT);
    udp->len = lwip_htons(UDP_HLEN + 4);
    struct pbuf *packet = pbuf_alloc(PBUF_RAW, length, PBUF_RAM);
    assert(packet && pbuf_take(packet, bytes, length) == ERR_OK);
    return packet;
}

/* How many times the socket got the datagram. */
static unsigned taken(struct netif *arrival, const char *source, const char *destination)
{
    delivered = 0;
    assert(ip_input(datagram(source, destination, IP_HLEN + UDP_HLEN + 4), arrival) == ERR_OK);
    return delivered;
}

int main(void)
{
    static struct netif local, second, tunnel;
    const char *host = "192.0.2.77", *peer = "100.101.102.50";
    lwip_init();
    add(&local, "192.0.2.10", "255.255.255.0", NULL, interface_ready);
    netif_set_link_up(&local);
    add(&second, "198.51.100.5", "255.255.255.0", NULL, interface_ready);
    netif_set_link_up(&second);
    struct udp_pcb *socket = udp_new();
    assert(socket && udp_bind(socket, IP_ANY_TYPE, PORT) == ERR_OK);
    udp_recv(socket, received, NULL);
    assert(taken(&local, host, "192.0.2.10") == 1);

    /* The tunnel as the device makes it: before the control server gave the
     * node its address, and with its link down, as while no peer has a
     * session. */
    struct wireguardif_init_data init = {.private_key = "c3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3M="};
    add(&tunnel, "100.64.0.1", "255.192.0.0", &init, wireguardif_init);
    assert(!netif_is_link_up(&tunnel));

    /* Why the rule exists: lwIP's own input takes a packet for the tunnel's
     * address that arrived on the local interface. */
    delivered = 0;
    assert(__real_ip4_input(datagram(host, "100.64.0.1", 32), &local) == ERR_OK && delivered == 1);

    assert(taken(&local, host, "100.64.0.1") == 0);
    assert(taken(&tunnel, peer, "100.64.0.1") == 1);
    /* The rule follows the address that the tunnel gets later. A control
     * server can hand out any range, so the source address plays no part: a
     * host that uses a tailnet address on the local network is stopped too. */
    readdress(&tunnel, "100.101.102.103");
    assert(taken(&local, host, "100.101.102.103") == 0);
    assert(taken(&local, peer, "100.101.102.103") == 0);
    assert(taken(&second, host, "100.101.102.103") == 0);
    assert(taken(&tunnel, peer, "100.101.102.103") == 1);
    readdress(&tunnel, "10.20.30.40");
    assert(taken(&local, host, "10.20.30.40") == 0 && taken(&tunnel, "10.20.30.50", "10.20.30.40") == 1);
    /* The rule is for the tunnel only: lwIP still takes the address of
     * another interface on each one, and each address on its own interface. */
    assert(taken(&local, host, "198.51.100.5") == 1);
    assert(taken(&local, host, "192.0.2.10") == 1 && taken(&tunnel, peer, "192.0.2.10") == 1);
    /* A packet that ends inside its IPv4 header is not read past its end. */
    assert(ip_input(datagram(host, "10.20.30.40", 12), &local) == ERR_OK);
    /* A tunnel without an address owns none: 0.0.0.0 is lwIP's to judge. */
    readdress(&tunnel, "0.0.0.0");
    assert(taken(&local, host, "0.0.0.0") == 1);

    /* The tunnel goes: its address belongs to no interface. */
    readdress(&tunnel, "100.101.102.103");
    wireguardif_shutdown(&tunnel);
    netif_remove(&tunnel);
    assert(taken(&local, host, "100.101.102.103") == 0 && taken(&local, host, "192.0.2.10") == 1);
    udp_remove(socket);
    puts("Tunnel input: the address of the tunnel is taken from the tunnel only: PASS");
    return 0;
}
