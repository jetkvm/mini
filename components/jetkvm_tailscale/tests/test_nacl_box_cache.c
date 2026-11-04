/* The NaCl box keeps the shared keys of recent key pairs. A sealed box must be
 * the same as one sealed with a freshly derived shared key, for every pair,
 * also after the pair's entry was evicted and when only one key differs. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nacl_box.h"
#include "x25519.h"

#define PAIRS 11 /* more than the 8 entries of a host build */
#define TEXT 40

static int failures;

static void key(uint8_t out[32], int seed) {
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(seed * 37 + i * 11 + 1);
}

static void check_pair(const uint8_t *public_key, const uint8_t *secret_key, int n) {
    uint8_t nonce[24], text[TEXT], cached[TEXT + NACL_BOX_MACBYTES], fresh[TEXT + NACL_BOX_MACBYTES];
    uint8_t shared[32], opened[TEXT];
    for (int i = 0; i < 24; i++) nonce[i] = (uint8_t)(n + i);
    for (int i = 0; i < TEXT; i++) text[i] = (uint8_t)(n * 3 + i);

    nacl_box(cached, text, TEXT, nonce, public_key, secret_key);
    nacl_box_beforenm(shared, public_key, secret_key);
    nacl_box_afternm(fresh, text, TEXT, nonce, shared);
    if (memcmp(cached, fresh, sizeof cached) != 0) {
        printf("FAIL pair %d: box differs from a fresh shared key\n", n);
        failures++;
    }
    if (nacl_box_open(opened, fresh, sizeof fresh, nonce, public_key, secret_key) != 0 ||
        memcmp(opened, text, TEXT) != 0) {
        printf("FAIL pair %d: open with the cached key\n", n);
        failures++;
    }
}

int main(void) {
    uint8_t public_keys[PAIRS][32], secret_keys[PAIRS][32], peer_secret[32];
    for (int n = 0; n < PAIRS; n++) {
        key(peer_secret, 100 + n);
        x25519_base(public_keys[n], peer_secret, 1);
        key(secret_keys[n], n);
    }
    /* More pairs than entries, twice, so every pair is evicted once. */
    for (int round = 0; round < 2; round++)
        for (int n = 0; n < PAIRS; n++) check_pair(public_keys[n], secret_keys[n], n);
    /* Same public key, other secret key, and the reverse. */
    check_pair(public_keys[0], secret_keys[1], 50);
    check_pair(public_keys[1], secret_keys[0], 51);
    check_pair(public_keys[0], secret_keys[0], 52);
    /* After the client erases its keys, the box derives every key anew. */
    nacl_box_forget();
    check_pair(public_keys[2], secret_keys[2], 53);
    if (failures) return 1;
    printf("ok   NaCl box shared-key cache (%d pairs)\n", PAIRS);
    return 0;
}
