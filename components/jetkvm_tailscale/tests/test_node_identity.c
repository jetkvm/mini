/* The node identity of microlink.c in its NVS namespace: the machine key,
 * which names the device to the control server, survives every start, also
 * one with a new node key, and only a log out or a reset erases it. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "x25519.h"

#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define NVS_READWRITE 1
#define NVS_NAMESPACE       "microlink"
#define NVS_KEY_MACHINE_PRI "machine_pri"
#define NVS_KEY_WG_PRI      "wg_private"
#define NVS_KEY_DISCO_PRI   "disco_pri"
typedef int nvs_handle_t;
typedef int nvs_open_mode_t;

static const char *TAG = "test_node_identity";

typedef struct {
    struct { bool renew_node_key; } config;
    uint8_t machine_private_key[32], machine_public_key[32];
    uint8_t wg_private_key[32], wg_public_key[32];
    uint8_t disco_private_key[32], disco_public_key[32];
} microlink_t;

/* The namespace: three 32-byte blobs. */
static const char *names[] = {NVS_KEY_MACHINE_PRI, NVS_KEY_WG_PRI, NVS_KEY_DISCO_PRI};
static struct { bool set; uint8_t value[32]; } blobs[3];
static unsigned commits, peer_cache_clears;

static int slot(const char *key)
{
    for (int i = 0; i < 3; ++i) if (!strcmp(key, names[i])) return i;
    assert(!"unexpected NVS key");
    return -1;
}
static esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, NVS_NAMESPACE) && mode == NVS_READWRITE);
    *handle = 1;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { (void)handle; }
static esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; ++commits; return ESP_OK; }
static esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    (void)handle;
    int i = slot(key);
    if (!blobs[i].set) return ESP_ERR_NVS_NOT_FOUND;
    assert(*length >= 32);
    memcpy(value, blobs[i].value, 32);
    *length = 32;
    return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    assert(length == 32);
    int i = slot(key);
    memcpy(blobs[i].value, value, 32);
    blobs[i].set = true;
    return ESP_OK;
}
static esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    (void)handle;
    memset(blobs, 0, sizeof(blobs));
    return ESP_OK;
}
static void ml_peer_nvs_init(void) {}
static void ml_peer_nvs_clear(void) { ++peer_cache_clears; }
static void ml_peer_nvs_deinit(void) {}
/* Random bytes that differ on every call. */
static uint8_t counter;
static void esp_fill_random(void *buffer, size_t length)
{
    ++counter;
    for (size_t i = 0; i < length; ++i) ((uint8_t *)buffer)[i] = (uint8_t)(counter * 31 + i);
}

#include IDENTITY_FUNCTIONS

typedef struct { uint8_t machine[32], node[32], disco[32]; } identity_t;

/* One start of the client: the keys it loads or makes. */
static identity_t start(bool renew_node_key)
{
    microlink_t ml = {.config.renew_node_key = renew_node_key};
    assert(load_or_generate_keys(&ml) == ESP_OK);
    identity_t keys;
    memcpy(keys.machine, ml.machine_public_key, 32);
    memcpy(keys.node, ml.wg_public_key, 32);
    memcpy(keys.disco, ml.disco_public_key, 32);
    /* The public key is derived from the stored private key. */
    uint8_t derived[32];
    x25519_base(derived, ml.machine_private_key, 1);
    assert(!memcmp(derived, keys.machine, 32));
    return keys;
}

int main(void)
{
    /* The first start makes and stores the three keys. */
    identity_t first = start(false);
    assert(blobs[0].set && blobs[1].set && blobs[2].set && commits == 1);

    /* A restart, an update, and Tailscale off and on with the key kept: the
     * same keys, and nothing written. */
    identity_t again = start(false);
    assert(!memcmp(&again, &first, sizeof(first)) && commits == 1);

    /* The key entered again, after Tailscale off removed it: a new node
     * key, and the same machine and DISCO keys. The control server finds
     * the device by its machine key and keeps it. */
    identity_t renewed = start(true);
    assert(!memcmp(renewed.machine, first.machine, 32));
    assert(!memcmp(renewed.disco, first.disco, 32));
    assert(memcmp(renewed.node, first.node, 32));
    assert(commits == 2);
    identity_t after = start(false);
    assert(!memcmp(&after, &renewed, sizeof(after)));

    /* A log out (and a factory reset, which erases all of NVS) erases the
     * namespace: the next start is a new device. */
    assert(microlink_factory_reset() == ESP_OK && peer_cache_clears == 1);
    identity_t fresh = start(false);
    assert(memcmp(fresh.machine, first.machine, 32) && memcmp(fresh.node, renewed.node, 32));
    puts("Tailscale node identity across restarts, re-enrollment and log out: PASS");
    return 0;
}
