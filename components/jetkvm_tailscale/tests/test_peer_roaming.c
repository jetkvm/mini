/* A peer changes from reachable LAN to DERP while its session stays valid. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "wireguardif.h"
#include "ml_peer_map.h"
#include PEER_ROAMING_TYPES

#define EXT_RAM_BSS_ATTR
#define DISCO_TXID_LEN 12
#define ML_DISCO_TRUST_DURATION_MS 15000
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)

typedef struct microlink {
    ml_peer_t peers[ML_MAX_PEERS];
    int peer_count;
    struct netif *wg_netif;
    void *config_httpd;
    int disco_sock4;
    struct { uint32_t priority_peer_ip; } config;
    void (*peer_cb)(struct microlink *, const microlink_peer_info_t *, void *);
    void *peer_cb_data;
} microlink_t;
typedef struct { bool via_derp; uint32_t src_ip; uint16_t src_port; } ml_rx_packet_t;

uint32_t sys_now(void) { return 1000; }
uint32_t wireguard_sys_now(void) { return sys_now(); }
void wireguard_random_bytes(void *data, size_t n) { memset(data, 0x42, n); }
void wireguard_tai64n_now(uint8_t *data) { memset(data, 0, 12); }
bool wireguard_is_under_load(void) { return false; }
static uint64_t ml_get_time_ms(void) { return sys_now(); }
static bool ml_lwip_lock(void) { return false; }
static void ml_lwip_unlock(bool acquired) { (void)acquired; }
static bool ml_config_peer_is_allowed(void *config, uint32_t ip) { (void)config; (void)ip; return true; }
static bool at_ready = true, udp_ready = true;
static unsigned cmm_replies, endpoint_probes, mapped_probes;
static uint32_t probed_ips[2];
static uint16_t probed_ports[2];
static bool ml_at_socket_is_ready(void) { return at_ready; }
static bool disco_has_udp_path(microlink_t *ml) { (void)ml; return udp_ready; }
static void microlink_ip_to_str(uint32_t ip, char *out) { (void)ip; strcpy(out, "100.127.240.4"); }
static void disco_send_call_me_maybe(microlink_t *ml, int idx) { (void)ml; (void)idx; ++cmm_replies; }
static void disco_send_ping_to_peer(microlink_t *ml, int idx, bool force) {
    (void)ml; (void)idx; (void)force; ++mapped_probes;
}
static void disco_build_ping(microlink_t *ml, int idx, uint8_t *data, size_t *len) {
    (void)ml; assert(idx == 0); memset(data, 0x5a, 32); *len = 32;
}
static int disco_udp_sendto(microlink_t *ml, const uint8_t *data, size_t len,
                            uint32_t ip, uint16_t port) {
    (void)ml; assert(len == 32 && data[0] == 0x5a);
    probed_ips[endpoint_probes % 2] = ip;
    probed_ports[endpoint_probes % 2] = port;
    ++endpoint_probes;
    return (int)len;
}
static int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *n,
                                 const unsigned char *key, size_t len) {
    *n = cap;
    return wireguard_base64_encode(key, len, (char *)out, n) ? 0 : -1;
}
#include PEER_ROAMING_FUNCTIONS

static unsigned direct_packets, relay_packets, handshake_packets;
static uint8_t expected_plain[32];
static struct wireguard_keypair receiver;
static uint8_t peer_key[32];
static uint32_t expected_endpoint = PP_HTONL(0xc0a80184);

static void verify_transport(const uint8_t *frame, size_t len) {
    assert(len == 64 && frame[0] == MESSAGE_TRANSPORT_DATA);
    const struct message_transport_data *header = (const void *)frame;
    assert(header->receiver == 5678);
    uint64_t counter = 0;
    for (unsigned i = 0; i < 8; ++i) counter |= (uint64_t)header->counter[i] << (8 * i);
    uint8_t plain[32];
    assert(wireguard_decrypt_packet(plain, header->enc_packet, 48, counter, &receiver));
    assert(!memcmp(plain, expected_plain, sizeof(plain)));
}
static err_t direct_output(uint32_t ip, uint16_t port, const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    assert(ip == expected_endpoint && port == 55058);
    /* Explicit session setup may also send an initiation. */
    if (data[0] == MESSAGE_HANDSHAKE_INITIATION) {
        assert(len == sizeof(struct message_handshake_initiation));
        ++handshake_packets;
        return ERR_OK;
    }
    verify_transport(data, len);
    ++direct_packets;
    return ERR_OK;
}
static err_t relay_output(const uint8_t *key, const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    assert(!memcmp(key, peer_key, 32));
    if (data[0] == MESSAGE_HANDSHAKE_INITIATION) {
        assert(len == sizeof(struct message_handshake_initiation));
        ++handshake_packets;
        return ERR_OK;
    }
    verify_transport(data, len);
    ++relay_packets;
    return ERR_OK;
}
static void send_packet(struct netif *netif) {
    struct pbuf *packet = pbuf_alloc(PBUF_RAW, sizeof(expected_plain), PBUF_RAM);
    assert(packet && pbuf_take(packet, expected_plain, sizeof(expected_plain)) == ERR_OK);
    ip4_addr_t dest;
    IP4_ADDR(&dest, 100, 127, 240, 4);
    assert(netif->output(netif, packet, &dest) == ERR_OK);
    pbuf_free(packet);
}
int main(void) {
    lwip_init();
    wireguardif_disable_socket_bind();
    struct wireguardif_init_data init = {
        .private_key = "c3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3M=",
    };
    struct netif netif = {.state = &init};
    assert(wireguardif_init(&netif) == ERR_OK);
    wireguardif_set_udp_output(&netif, direct_output, NULL);
    wireguardif_set_derp_output(&netif, relay_output, NULL);
    microlink_t ml = {.wg_netif = &netif};
    ml_peer_update_t update = {.node_id = 7, .vpn_ip = 0x647ff004, .endpoint_count = 1};
    strcpy(update.hostname, "roaming-peer");
    memset(update.public_key, 0x19, 32);
    memset(update.disco_key, 0x27, 32);
    memcpy(peer_key, update.public_key, 32);
    update.endpoints[0].ip = 0xc0a80184;
    update.endpoints[0].port = 55058;
    assert(add_peer(&ml, &update) == 0);
    struct wireguard_device *device = netif.state;
    struct wireguard_peer *peer = &device->peers[ml.peers[0].wg_peer_index];
    /* Discovery of an idle peer must not create a WireGuard session or start
     * retrying handshakes across the entire visible tailnet. */
    pending_probes[0] = (disco_probe_t){.active = true, .peer_index = 0, .sent_ms = 900};
    memset(pending_probes[0].txid, 0x42, DISCO_TXID_LEN);
    uint8_t pong[14] = {2, 0};
    memcpy(pong + 2, pending_probes[0].txid, DISCO_TXID_LEN);
    ml_rx_packet_t old_path = {.src_ip = 0xc0a80184, .src_port = 55058};
    process_disco_pong(&ml, &old_path, update.disco_key, pong, sizeof(pong));
    assert(handshake_packets == 0 && !peer->active);
    peer->curr_keypair = (struct wireguard_keypair){
        .valid = true, .initiator = true, .sending_valid = true,
        .receiving_valid = true, .keypair_millis = 1000,
        .local_index = 1234, .remote_index = 5678, .sending_counter = 19,
    };
    memset(peer->curr_keypair.sending_key, 0x73, 32);
    receiver.receiving_valid = true;
    memset(receiver.receiving_key, 0x73, 32);
    IP_ADDR4(&peer->ip, 192, 168, 1, 132);
    peer->port = 55058;
    peer->connect_ip = peer->ip;
    peer->connect_port = peer->port;
    ml.peers[0].has_direct_path = true;
    ml.peers[0].best_ip = update.endpoints[0].ip;
    ml.peers[0].best_port = 55058;
    ml.peers[0].trust_until_ms = 16000;
    expected_plain[0] = 0x45;
    expected_plain[3] = 28;
    expected_plain[9] = 17;
    memcpy(expected_plain + 12, (uint8_t[]){100,127,240,5,100,127,240,4}, 8);
    expected_plain[23] = 80;
    send_packet(&netif);
    assert(direct_packets == 1 && relay_packets == 0);
    struct wireguard_keypair session = peer->curr_keypair;
    pending_probes[0] = (disco_probe_t){.active = true, .peer_index = 0, .sent_ms = 900};
    memset(pending_probes[0].txid, 0x42, DISCO_TXID_LEN);
    memcpy(pong + 2, pending_probes[0].txid, DISCO_TXID_LEN);
    update.endpoints[0].ip = 0x5001575c; /* New mobile network. */
    assert(add_peer(&ml, &update) == 0);
    assert(!memcmp(&session, &peer->curr_keypair, sizeof(session)));
    send_packet(&netif);
    assert(direct_packets == 1 && relay_packets == 1);
    process_disco_pong(&ml, &old_path, update.disco_key, pong, sizeof(pong));
    send_packet(&netif);
    assert(direct_packets == 1 && relay_packets == 2);
    /* A transaction from a different authenticated discovery key is rejected. */
    pending_probes[0].active = true;
    uint8_t wrong_key[32] = {0};
    process_disco_pong(&ml, &old_path, wrong_key, pong, sizeof(pong));
    send_packet(&netif);
    assert(direct_packets == 1 && relay_packets == 3);
    /* A new, correctly authenticated probe restores direct encrypted output. */
    pending_probes[0].active = true;
    ml_rx_packet_t new_path = {.src_ip = update.endpoints[0].ip, .src_port = 55058};
    expected_endpoint = lwip_htonl(new_path.src_ip);
    session = peer->curr_keypair;
    process_disco_pong(&ml, &new_path, update.disco_key, pong, sizeof(pong));
    assert(handshake_packets == 0);
    assert(!memcmp(&session, &peer->curr_keypair, sizeof(session)));
    send_packet(&netif);
    assert(direct_packets == 2 && relay_packets == 3);
    /* Losing direct-path trust must also retire the configured endpoint.
     * Otherwise a later reconnect restores the expired LAN route. */
    assert(wireguardif_connect_derp(&netif, (u8_t)ml.peers[0].wg_peer_index) == ERR_OK);
    assert(wireguardif_connect(&netif, (u8_t)ml.peers[0].wg_peer_index) == ERR_ARG);
    send_packet(&netif);
    assert(direct_packets == 2 && relay_packets == 4);
    /* Repeated endpoint invitations must produce UDP probes, not a relay
     * reply that the other embedded peer will reflect back indefinitely. */
    uint8_t invitation[38] = {
        3, 0,
        0,0,0,0,0,0,0,0,0,0,0xff,0xff, 192,168,1,132, 0xd7,0x12,
        0,0,0,0,0,0,0,0,0,0,0xff,0xff, 80,1,87,92, 0xa2,0x8c,
    };
    ml_rx_packet_t relay_path = {.via_derp = true};
    at_ready = false;
    cmm_replies = endpoint_probes = mapped_probes = 0;
    for (unsigned i = 0; i < 100; ++i)
        process_disco_call_me_maybe(&ml, &relay_path, update.disco_key,
                                    invitation, sizeof(invitation));
    assert(cmm_replies == 0 && endpoint_probes == 200 && mapped_probes == 100);
    assert(probed_ips[0] == 0xc0a80184 && probed_ports[0] == 55058);
    assert(probed_ips[1] == 0x5001575c && probed_ports[1] == 41612);
    endpoint_probes = mapped_probes = 0;
    process_disco_call_me_maybe(&ml, &old_path, update.disco_key, invitation, sizeof(invitation));
    process_disco_call_me_maybe(&ml, &relay_path, wrong_key, invitation, sizeof(invitation));
    process_disco_call_me_maybe(&ml, &relay_path, update.disco_key, invitation, sizeof(invitation)-1);
    assert(cmm_replies == 0 && endpoint_probes == 0 && mapped_probes == 0);
    invitation[18] = invitation[19] = 0; /* Ignore zero-port endpoints. */
    process_disco_call_me_maybe(&ml, &relay_path, update.disco_key, invitation, sizeof(invitation));
    assert(endpoint_probes == 1 && probed_ips[0] == 0x5001575c);
    puts("Endpoint invitations: UDP probes, no recursive relay replies, invalid sources rejected: PASS");
    /* The relay pong of a probe that also went over UDP comes first: the
     * direct pong of the same transaction still sets the direct path. */
    ml.peers[0].has_direct_path = false;
    pending_probes[0] = (disco_probe_t){.active = true, .peer_index = 0, .sent_ms = 900};
    memset(pending_probes[0].txid, 0x42, DISCO_TXID_LEN);
    process_disco_pong(&ml, &relay_path, update.disco_key, pong, sizeof(pong));
    assert(pending_probes[0].active && !ml.peers[0].has_direct_path);
    process_disco_pong(&ml, &new_path, update.disco_key, pong, sizeof(pong));
    assert(!pending_probes[0].active && ml.peers[0].has_direct_path);
    puts("Relay pong first: the direct pong of the same probe sets the direct path: PASS");
    pending_probes[0].active = true;
    remove_peer(&ml, &update);
    assert(!pending_probes[0].active);
    wireguardif_shutdown(&netif);
    puts("Peer mobility: encrypted replies move from stale LAN to DERP; late pongs rejected: PASS");
}
