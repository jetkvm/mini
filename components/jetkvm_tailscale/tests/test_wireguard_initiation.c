/* The responder's limits on handshake initiations, with real Noise messages. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wireguard.h"
#include "crypto.h"

static uint32_t now_ms = 10000;
static uint64_t tai64n_counter;

uint32_t sys_now(void) { return now_ms; }
uint32_t wireguard_sys_now(void) { return now_ms; }
bool wireguard_is_under_load(void) { return false; }

void wireguard_random_bytes(void *data, size_t length)
{
    static uint8_t next = 1;
    for (size_t i = 0; i < length; ++i) ((uint8_t *)data)[i] = next++;
}

/* Each initiation carries a greater timestamp, as a peer's clock gives. */
void wireguard_tai64n_now(uint8_t *data)
{
    ++tai64n_counter;
    memset(data, 0, 12);
    for (unsigned i = 0; i < 8; ++i) data[7 - i] = (uint8_t)(tai64n_counter >> (8 * i));
}

static struct wireguard_device mini, laptop;

static bool laptop_initiation_accepted(void)
{
    struct message_handshake_initiation message;
    assert(wireguard_create_handshake_initiation(&laptop, &laptop.peers[0], &message));
    return wireguard_process_initiation_message(&mini, &message) == &mini.peers[0];
}

/* An initiation whose mac1 is valid but whose keys are nobody's: the mac1
 * key is a hash of the Mini's public key. */
static void forged_initiation(unsigned seed)
{
    struct message_handshake_initiation message;
    memset(&message, (int)seed, sizeof(message));
    message.type = MESSAGE_HANDSHAKE_INITIATION;
    memset(message.reserved, 0, sizeof(message.reserved));
    blake2s(message.mac1, WIREGUARD_COOKIE_LEN, mini.label_mac1_key, WIREGUARD_SESSION_KEY_LEN,
            &message, sizeof(message) - 2 * WIREGUARD_COOKIE_LEN);
    assert(wireguard_check_mac1(&mini, (uint8_t *)&message, sizeof(message) - 2 * WIREGUARD_COOKIE_LEN, message.mac1));
    assert(!wireguard_process_initiation_message(&mini, &message));
}

int main(void)
{
    uint8_t mini_key[WIREGUARD_PRIVATE_KEY_LEN], laptop_key[WIREGUARD_PRIVATE_KEY_LEN];
    memset(mini_key, 0x11, sizeof(mini_key));
    memset(laptop_key, 0x22, sizeof(laptop_key));
    wireguard_init();
    assert(wireguard_device_init(&mini, mini_key) && wireguard_device_init(&laptop, laptop_key));
    assert(wireguard_peer_init(&mini, &mini.peers[0], laptop.public_key, NULL));
    assert(wireguard_peer_init(&laptop, &laptop.peers[0], mini.public_key, NULL));

    /* A peer gets at most MAX_INITIATIONS_PER_SECOND accepted initiations. */
    assert(laptop_initiation_accepted());
    now_ms += 100;
    assert(!laptop_initiation_accepted());
    now_ms += 1000 / MAX_INITIATIONS_PER_SECOND;
    assert(laptop_initiation_accepted());

    /* Initiations that nobody authenticates take the device's X25519 at most
     * MAX_DEVICE_INITIATIONS_PER_SECOND times a second: the rest are dropped
     * before it, so in that second a peer's initiation is dropped too. In the
     * next second the peer's initiation is accepted. */
    now_ms += 1000;
    for (unsigned i = 0; i < 4 * MAX_DEVICE_INITIATIONS_PER_SECOND; ++i) forged_initiation(0x40 + i);
    assert(!laptop_initiation_accepted());
    now_ms += 1000;
    assert(laptop_initiation_accepted());
    puts("WireGuard initiations: per-peer rate limit and device budget before X25519: PASS");
}
