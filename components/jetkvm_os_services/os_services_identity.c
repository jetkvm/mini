/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_identity.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char *TAG = "osvc_identity";

#define IDENTITY_NAMESPACE "jk_osvc"
#define IDENTITY_SEED_KEY "seed"
#define SEED_LEN 32U

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_loaded;
static uint8_t s_secret_key[64];
static uint8_t s_public_key[OSVC_PUBLIC_KEY_LEN];
static char s_fingerprint[OSVC_FINGERPRINT_TEXT_CAPACITY];

static esp_err_t read_seed(uint8_t seed[SEED_LEN])
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(IDENTITY_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    size_t length = SEED_LEN;
    error = nvs_get_blob(handle, IDENTITY_SEED_KEY, seed, &length);
    nvs_close(handle);
    if (error == ESP_OK && length != SEED_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    return error;
}

static esp_err_t write_seed(const uint8_t seed[SEED_LEN])
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, IDENTITY_SEED_KEY, seed, SEED_LEN);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t osvc_identity_load(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool loaded = s_loaded;
    portEXIT_CRITICAL(&s_lock);
    if (loaded) {
        return ESP_OK;
    }

    uint8_t seed[SEED_LEN];
    esp_err_t error = read_seed(seed);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        /* The hardware RNG, as for the firmware's other long-lived keys. */
        esp_fill_random(seed, sizeof(seed));
        error = write_seed(seed);
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "made a new OS Services identity key");
        }
    }
    if (error != ESP_OK) {
        crypto_wipe(seed, sizeof(seed));
        ESP_LOGE(TAG, "OS Services identity key unavailable: %s",
                 esp_err_to_name(error));
        return error;
    }

    uint8_t secret_key[64];
    uint8_t public_key[OSVC_PUBLIC_KEY_LEN];
    /* Wipes the seed. */
    crypto_ed25519_key_pair(secret_key, public_key, seed);

    uint8_t digest[32];
    size_t digest_length = 0U;
    psa_status_t result = psa_crypto_init();
    if (result == PSA_SUCCESS) {
        result = psa_hash_compute(PSA_ALG_SHA_256, public_key,
                                  sizeof(public_key), digest, sizeof(digest),
                                  &digest_length);
    }
    if (result != PSA_SUCCESS || digest_length != sizeof(digest)) {
        crypto_wipe(secret_key, sizeof(secret_key));
        return ESP_FAIL;
    }
    char fingerprint[OSVC_FINGERPRINT_TEXT_CAPACITY];
    osvc_format_fingerprint(digest, fingerprint);

    portENTER_CRITICAL(&s_lock);
    memcpy(s_secret_key, secret_key, sizeof(s_secret_key));
    memcpy(s_public_key, public_key, sizeof(s_public_key));
    memcpy(s_fingerprint, fingerprint, sizeof(s_fingerprint));
    s_loaded = true;
    portEXIT_CRITICAL(&s_lock);
    crypto_wipe(secret_key, sizeof(secret_key));
    ESP_LOGI(TAG, "OS Services key fingerprint %s", fingerprint);
    return ESP_OK;
}

bool osvc_identity_public_key(uint8_t key[OSVC_PUBLIC_KEY_LEN])
{
    portENTER_CRITICAL(&s_lock);
    const bool loaded = s_loaded;
    if (loaded) {
        memcpy(key, s_public_key, OSVC_PUBLIC_KEY_LEN);
    }
    portEXIT_CRITICAL(&s_lock);
    return loaded;
}

bool osvc_identity_sign(const uint8_t *message, size_t length,
                        uint8_t signature[OSVC_SIGNATURE_LEN])
{
    uint8_t secret_key[64];
    portENTER_CRITICAL(&s_lock);
    const bool loaded = s_loaded;
    if (loaded) {
        memcpy(secret_key, s_secret_key, sizeof(secret_key));
    }
    portEXIT_CRITICAL(&s_lock);
    if (!loaded) {
        return false;
    }
    crypto_ed25519_sign(signature, secret_key, message, length);
    crypto_wipe(secret_key, sizeof(secret_key));
    return true;
}

void osvc_identity_fingerprint(char out[OSVC_FINGERPRINT_TEXT_CAPACITY])
{
    portENTER_CRITICAL(&s_lock);
    memcpy(out, s_fingerprint, OSVC_FINGERPRINT_TEXT_CAPACITY);
    portEXIT_CRITICAL(&s_lock);
}
