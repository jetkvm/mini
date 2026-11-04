/* Exercise real WireGuard transport decryption and netif input delivery. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "wireguardif.h"
#include "crypto.h"
#include "ml_packet_filter.h"

static unsigned delivered, accepted;
static bool queue_full;
static cJSON *filters;
static const uint32_t local_ip = 0x647ff005;

uint32_t sys_now(void) { return 1000; }
uint32_t wireguard_sys_now(void) { return sys_now(); }
void wireguard_random_bytes(void *data, size_t length) { memset(data, 0x42, length); }
void wireguard_tai64n_now(uint8_t *data) { memset(data, 0, 12); }
bool wireguard_is_under_load(void) { return false; }

static err_t filtered_input(struct pbuf *packet, struct netif *netif)
{
    (void)netif;
    ++delivered;
    if (queue_full) return ERR_MEM; /* Caller must release this packet. */
    if (ml_packet_filter_accept(filters, packet->payload, packet->tot_len, local_ip)) ++accepted;
    pbuf_free(packet);
    return ERR_OK;
}

static void encrypt(uint8_t *frame, uint32_t receiver, struct wireguard_keypair *sender,
                    const uint8_t *plain, size_t length)
{
    struct message_transport_data *header = (void *)frame;
    frame[0] = MESSAGE_TRANSPORT_DATA;
    header->receiver = receiver;
    for (unsigned i = 0; i < 8; ++i) header->counter[i] = sender->sending_counter >> (8 * i);
    wireguard_encrypt_packet(header->enc_packet, plain, length, sender);
}

/* Handshake-shaped datagrams from anyone: random ones, and ones whose mac1
 * is valid, since mac1 needs only the device's public key. */
static long console_bytes_for_unauthenticated_handshakes(struct netif *netif)
{
    struct wireguard_device *device = netif->state;
    fflush(stdout);
    FILE *console = tmpfile();
    int saved = dup(STDOUT_FILENO);
    assert(console && saved >= 0 && dup2(fileno(console), STDOUT_FILENO) >= 0);
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t initiation[sizeof(struct message_handshake_initiation)];
        uint8_t response[sizeof(struct message_handshake_response)];
        memset(initiation, 0x30 + i, sizeof(initiation));
        memset(response, 0x50 + i, sizeof(response));
        initiation[0] = MESSAGE_HANDSHAKE_INITIATION;
        response[0] = MESSAGE_HANDSHAKE_RESPONSE;
        memset(initiation + 1, 0, 3);
        memset(response + 1, 0, 3);
        if (i & 1) {
            struct message_handshake_initiation *init = (void *)initiation;
            struct message_handshake_response *resp = (void *)response;
            blake2s(init->mac1, WIREGUARD_COOKIE_LEN, device->label_mac1_key, WIREGUARD_SESSION_KEY_LEN,
                    initiation, sizeof(initiation) - 2 * WIREGUARD_COOKIE_LEN);
            blake2s(resp->mac1, WIREGUARD_COOKIE_LEN, device->label_mac1_key, WIREGUARD_SESSION_KEY_LEN,
                    response, sizeof(response) - 2 * WIREGUARD_COOKIE_LEN);
        }
        assert(wireguardif_inject_packet(netif, 0x0a000002u, 40000, initiation, sizeof(initiation)) == ERR_OK);
        assert(wireguardif_inject_packet(netif, 0x0a000002u, 40000, response, sizeof(response)) == ERR_OK);
    }
    fflush(stdout);
    struct stat written;
    assert(fstat(fileno(console), &written) == 0);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    fclose(console);
    return (long)written.st_size;
}

static void receive(struct netif *netif, struct wireguard_keypair *sender,
                    const uint8_t *plain, size_t length, bool corrupt)
{
    uint8_t frame[16 + 128 + WIREGUARD_AUTHTAG_LEN] = {0};
    encrypt(frame, 1234, sender, plain, length);
    if (corrupt) frame[16 + length] ^= 1;
    assert(wireguardif_inject_packet(netif, 0, 0, frame,
                                    16 + length + WIREGUARD_AUTHTAG_LEN) == ERR_OK);
}

int main(void)
{
    lwip_init();
    struct netif netif = {.input = filtered_input};
    struct wireguard_device device = {.valid = true, .netif = &netif};
    netif.state = &device;
    struct wireguard_peer *peer = &device.peers[0];
    peer->valid = true;
    peer->curr_keypair = (struct wireguard_keypair){
        .valid = true, .receiving_valid = true, .keypair_millis = 1000, .local_index = 1234,
    };
    memset(peer->curr_keypair.receiving_key, 0x73, 32);
    peer->allowed_source_ips[0].valid = true;
    IP_ADDR4(&peer->allowed_source_ips[0].ip, 100, 127, 240, 4);
    IP_ADDR4(&peer->allowed_source_ips[0].mask, 255, 255, 255, 255);
    struct wireguard_keypair sender = {.sending_valid = true};
    memset(sender.sending_key, 0x73, 32);
    uint8_t packet[32] = {
        0x45, 0, 0, 28, 0, 0, 0, 0, 64, 17, 0, 0,
        100, 127, 240, 4, 100, 127, 240, 5,
        0x13, 0x88, 0, 80, 0, 8, 0, 0,
    };
    receive(&netif, &sender, packet, sizeof(packet), false);
    assert(delivered == 1 && accepted == 0); /* No policy: deny. */
    cJSON *map = cJSON_Parse("{\"PacketFilter\":[{\"SrcIPs\":[\"100.127.240.4\"],\"DstPorts\":[{\"IP\":\"100.127.240.5\",\"Ports\":{\"First\":80,\"Last\":80}}],\"IPProto\":[17]}]}");
    assert(map && ml_packet_filter_update(&filters, map));
    cJSON_Delete(map);
    receive(&netif, &sender, packet, sizeof(packet), false);
    assert(delivered == 2 && accepted == 1);
    packet[23] = 81;
    receive(&netif, &sender, packet, sizeof(packet), false);
    assert(delivered == 3 && accepted == 1); /* Denied port reaches the filter. */
    packet[15] = 9;
    receive(&netif, &sender, packet, sizeof(packet), false);
    assert(delivered == 3); /* Authenticated peer cannot spoof a source. */
    packet[15] = 4;
    receive(&netif, &sender, packet, sizeof(packet), true);
    assert(delivered == 3); /* Bad authentication never reaches input. */
    receive(&netif, &sender, packet, 16, false);
    assert(delivered == 3); /* A truncated IPv4 header is dropped safely. */
    queue_full = true;
    receive(&netif, &sender, packet, sizeof(packet), false);
    assert(delivered == 4 && accepted == 1);
    queue_full = false;
    /* The first packet with the key of a responder handshake promotes the
     * next keypair to the current one. A second copy of that datagram is a
     * replay and must not reach the interface. */
    peer->next_keypair = (struct wireguard_keypair){
        .valid = true, .receiving_valid = true, .keypair_millis = 1000, .local_index = 5678,
    };
    memset(peer->next_keypair.receiving_key, 0x5a, 32);
    struct wireguard_keypair responder = {.sending_valid = true};
    memset(responder.sending_key, 0x5a, 32);
    uint8_t frame[16 + 128 + WIREGUARD_AUTHTAG_LEN] = {0};
    encrypt(frame, 5678, &responder, packet, sizeof(packet));
    size_t frame_length = 16 + sizeof(packet) + WIREGUARD_AUTHTAG_LEN;
    uint8_t copy[sizeof(frame)];
    memcpy(copy, frame, sizeof(frame));
    assert(wireguardif_inject_packet(&netif, 0, 0, frame, frame_length) == ERR_OK);
    assert(delivered == 5 && peer->curr_keypair.local_index == 5678 && !peer->next_keypair.valid);
    assert(wireguardif_inject_packet(&netif, 0, 0, copy, frame_length) == ERR_OK);
    assert(delivered == 5);
    /* A replayed datagram, data or keepalive, from another address changes
     * nothing: the peer's endpoint stays where its new datagrams come from. */
    const uint32_t endpoint = PP_HTONL(0xc0000201), other = PP_HTONL(0xc6336407);
    for (size_t length = sizeof(packet);; length = 0) {
        encrypt(frame, 5678, &responder, packet, length);
        memcpy(copy, frame, sizeof(frame));
        size_t sent = 16 + length + WIREGUARD_AUTHTAG_LEN;
        assert(wireguardif_inject_packet(&netif, endpoint, 41641, frame, sent) == ERR_OK);
        assert(ip4_addr_get_u32(ip_2_ip4(&peer->ip)) == endpoint && peer->port == 41641);
        assert(wireguardif_inject_packet(&netif, other, 40000, copy, sent) == ERR_OK);
        assert(ip4_addr_get_u32(ip_2_ip4(&peer->ip)) == endpoint && peer->port == 41641);
        if (!length) break;
    }
    assert(delivered == 6);
    cJSON_Delete(filters);
    /* Real initialization owns a large peer table. Repeated stop must release
     * it and remain safe when called again; LeakSanitizer checks the allocation. */
    wireguardif_disable_socket_bind();
    for (unsigned i = 0; i < 3; ++i) {
        struct wireguardif_init_data init = {
            .private_key = "c3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3M=",
        };
        struct netif owned = {.state = &init, .input = filtered_input};
        assert(wireguardif_init(&owned) == ERR_OK && owned.state);
        wireguardif_shutdown(&owned);
        assert(!owned.state);
        wireguardif_shutdown(&owned);
    }
    /* The console is a blocking UART: handshake datagrams that do not
     * complete a handshake write nothing to it. */
    struct wireguardif_init_data init = {
        .private_key = "c3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3M=",
    };
    struct netif quiet = {.state = &init, .input = filtered_input};
    assert(wireguardif_init(&quiet) == ERR_OK && quiet.state);
    assert(console_bytes_for_unauthenticated_handshakes(&quiet) == 0);
    wireguardif_shutdown(&quiet);
    puts("WireGuard decryption, input grants, malformed frames, replays after a key promotion, replays from another address, queue ownership and silent handshake rejection: PASS");
}
