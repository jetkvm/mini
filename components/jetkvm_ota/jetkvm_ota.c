/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "jetkvm_board_config.h"
#include "jetkvm_device_state.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_ota_signature.h"
#include "jetkvm_time_sync.h"
#include "os_services_pgp_psa.h"

/* A bench build that tests against a stand-in cloud (JETKVM_MOCK_CLOUD)
 * trusts the test release key, whose private half is in the repository, for
 * every update server but the real one, and asks that server for the key at
 * /keyserver/<fingerprint>. tools/release_r2.sh refuses such an image. */
#ifndef JETKVM_MOCK_CLOUD
#define JETKVM_MOCK_CLOUD 0
#endif
#if JETKVM_MOCK_CLOUD
#include "jetkvm_ota_test_key.h"
#endif

#define TAG "jetkvm_ota"

/* Byte offset of the custom descriptor inside an app image, per ESP-IDF's
 * app image format: image header, first segment header, esp_app_desc_t.
 * The descriptor is read from the first download chunk, so a wrong-board
 * image is refused before anything is written to the slot. */
/* Returned by http_guard_init when the memory policy refuses the
 * connection's reservation. Distinct from ESP_ERR_NO_MEM, which the fetch and download
 * paths return for their own buffer allocations, so the two get different
 * messages. Outside ESP-IDF's error bases (the highest is 0xd000). */
#define OTA_ERR_TLS_ADMISSION ((esp_err_t)0x1a000)
#define OTA_ERROR_TLS_ADMISSION \
    "not enough free memory for a secure connection to the update server"
#define OTA_ERROR_NO_MEMORY_CHECK "not enough free memory to check for updates"
#define OTA_ERROR_NO_MEMORY_DOWNLOAD "not enough free memory to download the update"
/* The prefixes of jetkvm/kvm's update errors (internal/ota/sys.go). */
#define OTA_ERROR_SIGNATURE "Error with system signature: "
#define OTA_ERROR_VERIFY "Error verifying system update: "
/* Returned by install_system when it has written the reason itself. */
#define OTA_ERR_REPORTED ((esp_err_t)0x1a001)
#define OTA_IMAGE_DESC_OFFSET \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + \
     sizeof(esp_app_desc_t))
#define OTA_IMAGE_HEAD_BYTES \
    (OTA_IMAGE_DESC_OFFSET + sizeof(jetkvm_ota_image_desc_t))

_Static_assert(sizeof(jetkvm_ota_image_desc_t) == 64U,
               "image descriptor layout is part of the image format");
_Static_assert(sizeof(JETKVM_DEVICE_SKU) <= JETKVM_OTA_IMAGE_SKU_CAPACITY,
               "SKU does not fit the image descriptor");

const jetkvm_ota_image_desc_t jetkvm_image_desc
    __attribute__((section(".rodata_custom_desc"))) = {
    .magic = JETKVM_OTA_IMAGE_DESC_MAGIC,
    .sku = JETKVM_DEVICE_SKU,
};

#define OTA_NVS_NAMESPACE "jkvm_ota"
#define OTA_NVS_KEY "config"
#define OTA_RECORD_MAGIC 0x41544f4aU /* "JOTA" little-endian */
#define OTA_RECORD_VERSION 1U

#define OTA_METADATA_BUFFER_BYTES 4096U
/* One flash erase block. The slot is written in whole blocks from offset 0,
 * so sequential-writes mode erases each with a single 64 KiB block erase
 * instead of sixteen 4 KiB sector erases, which take several times longer
 * in total. */
#define OTA_DOWNLOAD_CHUNK_BYTES 65536U
/* Used when the control arena has no 64 KiB block free; the write is then
 * slower because each chunk is smaller than one erase block. */
#define OTA_DOWNLOAD_CHUNK_FALLBACK_BYTES 8192U
#define OTA_HTTP_TIMEOUT_MS 20000
/* An update check runs on the update RPC worker (QUEUE_UPDATE in
 * rpc_worker.c), so its wait holds up no other request; a second check
 * meanwhile is answered with "another update check is active". A lost TCP
 * segment of the TLS handshake is sent again about 1.5 s into the check and
 * once more at about 4.5 s, so the timeout leaves room for that second
 * resend. Downloads run on the OTA task and get the long one. */
#define OTA_CHECK_TIMEOUT_MS 8000
/* Internal DMA RAM, which every TLS admission is judged against. An install
 * over HTTP or HTTPS, including one that fails verification, peaked at
 * 5,153 B. */
#define OTA_TASK_STACK_BYTES 8192U
/* A side download's reads and handshakes time out after 30 seconds. */
#define OTA_SIDE_STOP_WAIT_MS 35000
#define OTA_TASK_PRIORITY 4
#define OTA_AUTO_UPDATE_FIRST_DELAY_US (15LL * 60LL * 1000000LL)
#define OTA_AUTO_UPDATE_INTERVAL_US (60LL * 60LL * 1000000LL)
#define OTA_PROGRESS_PUSH_DIVISOR 100
/* Unsynced clocks still need monotonic, non-null timestamps because the web
 * UI derives the install stage from null-vs-non-null alone. */
#define OTA_PSEUDO_EPOCH_UTC 1767225600LL /* 2026-01-01T00:00:00Z */

typedef struct {
    uint32_t magic;
    uint32_t version;
    jetkvm_ota_config_t config;
    uint32_t crc32;
} ota_record_t;

typedef struct {
    char app_constraint[JETKVM_OTA_VERSION_CAPACITY];
    char system_constraint[JETKVM_OTA_VERSION_CAPACITY];
    bool include_prerelease;
    bool reset_config;
    bool pending;
} ota_request_t;

static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool stopping;
    jetkvm_ota_hooks_t hooks;
    jetkvm_ota_config_t config;
    jetkvm_ota_state_t state;
    jetkvm_ota_update_status_t status;
    bool status_valid;
    ota_request_t request;
    bool auto_update_armed;
    int64_t next_auto_update_us;
    /* A firmware install runs; no side download may start. */
    bool installing;
    bool side_active;
    void (*side_stop)(void *ctx);
    void *side_ctx;
    /* The release key's signing keys, as the keyservers last served them,
     * for the firmware update and the OS Services releases. Under
     * keys_lock, which a fetch holds. */
    SemaphoreHandle_t keys_lock;
    jetkvm_ota_release_keys_t release_keys;
    /* The OTA task's copy, for the image that it installs. */
    osvc_pgp_key_t signing_keys[JETKVM_OTA_SIGNING_KEYS_MAX];
    size_t signing_key_count;
} s_ota;

static uint32_t ota_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

static void config_defaults(jetkvm_ota_config_t *config)
{
    memset(config, 0, sizeof(*config));
    (void)snprintf(config->update_api_url, sizeof(config->update_api_url),
                   "%s", JETKVM_OTA_DEFAULT_API_URL);
    config->auto_update_enabled = true;
}

static esp_err_t config_load(jetkvm_ota_config_t *config)
{
    config_defaults(config);
    ota_record_t record = {0};
    size_t size = sizeof(record);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, OTA_NVS_KEY, &record, &size);
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(record) || record.magic != OTA_RECORD_MAGIC ||
        record.version != OTA_RECORD_VERSION ||
        record.crc32 != ota_crc32(&record, offsetof(ota_record_t, crc32))) {
        ESP_LOGW(TAG, "ignoring malformed persisted OTA config");
        return ESP_OK;
    }
    if (record.config.update_api_url[0] != '\0' &&
        !jetkvm_ota_validate_api_url(record.config.update_api_url)) {
        ESP_LOGW(TAG, "ignoring persisted invalid update API URL");
        return ESP_OK;
    }
    *config = record.config;
    /* Records written before the default existed carry an empty URL. */
    if (config->update_api_url[0] == '\0') {
        (void)snprintf(config->update_api_url, sizeof(config->update_api_url),
                       "%s", JETKVM_OTA_DEFAULT_API_URL);
    }
    return ESP_OK;
}

static esp_err_t config_save(const jetkvm_ota_config_t *config)
{
    ota_record_t record = {
        .magic = OTA_RECORD_MAGIC,
        .version = OTA_RECORD_VERSION,
        .config = *config,
    };
    record.crc32 = ota_crc32(&record, offsetof(ota_record_t, crc32));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, OTA_NVS_KEY, &record, sizeof(record));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    memset(&record, 0, sizeof(record));
    return err;
}

/** Monotonic ISO8601 timestamp: real UTC when NTP-synced, otherwise a
 * deterministic pseudo-clock anchored at 2026-01-01 plus uptime. */
static void current_timestamp(char *output, size_t capacity)
{
    int64_t epoch;
    if (jetkvm_time_sync_ready()) {
        epoch = (int64_t)time(NULL);
    } else {
        epoch = OTA_PSEUDO_EPOCH_UTC + esp_timer_get_time() / 1000000LL;
    }
    time_t value = (time_t)epoch;
    struct tm broken_down;
    if (gmtime_r(&value, &broken_down) == NULL ||
        strftime(output, capacity, "%Y-%m-%dT%H:%M:%SZ", &broken_down) == 0U) {
        (void)snprintf(output, capacity, "2026-01-01T00:00:00Z");
    }
}

static void notify_state_listener(void)
{
    if (s_ota.hooks.state_listener) {
        s_ota.hooks.state_listener(s_ota.hooks.state_listener_ctx);
    }
}

static void set_error(const char *error)
{
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    (void)snprintf(s_ota.state.error, sizeof(s_ota.state.error), "%s", error);
    s_ota.state.updating = false;
    s_ota.installing = false;
    xSemaphoreGive(s_ota.lock);
    ESP_LOGW(TAG, "update failed: %s", error);
    notify_state_listener();
}

void jetkvm_ota_local_versions(char *app_version, size_t app_capacity,
                               char *system_version, size_t system_capacity)
{
    const esp_app_desc_t *description = esp_app_get_description();
    (void)snprintf(system_version, system_capacity, "%s",
                   description ? description->version : "unknown");
    if (app_capacity > 0U) app_version[0] = '\0';
}

static void compute_availability(jetkvm_ota_update_status_t *status,
                                 const char *system_constraint)
{
    status->app_update_available = false; /* no app artifact on this product */
    status->system_update_available = jetkvm_ota_update_available(
        status->local_system_version, status->remote.system_version,
        system_constraint);
}

/* ---- HTTP download helpers ---- */

typedef struct {
    jetkvm_memory_policy_reservation_t memory;
} ota_http_guard_t;

/* image is true for the firmware image: the Wi-Fi link's blocks wait while
 * flash is written, so its download reserves them beside its connection,
 * also over plain HTTP. The update check and the download wait for other
 * owners' reservations to end instead of failing over them. */
static esp_err_t http_guard_init(ota_http_guard_t *guard, const char *url,
                                 bool image)
{
    memset(guard, 0, sizeof(*guard));
    const bool tls = strncmp(url, "https://", 8) == 0;
    if (image ?
            !jetkvm_memory_policy_wait_reserve_download("ota-download",
                                                        &guard->memory) :
            tls && !jetkvm_memory_policy_wait_reserve_tls("ota-download",
                                                          &guard->memory)) {
        /* Without TLS the refusal is for the download's own memory. */
        return tls ? OTA_ERR_TLS_ADMISSION : ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void http_guard_done(ota_http_guard_t *guard)
{
    jetkvm_memory_policy_release(&guard->memory);
}

static esp_err_t http_open(esp_http_client_handle_t *client, const char *url,
                           int timeout_ms)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    *client = esp_http_client_init(&config);
    if (*client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(*client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(*client);
        *client = NULL;
        return err;
    }
    int64_t length = esp_http_client_fetch_headers(*client);
    if (length < 0) {
        esp_http_client_close(*client);
        esp_http_client_cleanup(*client);
        *client = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t fetch_metadata(const char *url, int timeout_ms,
                                jetkvm_ota_metadata_t *metadata)
{
    ota_http_guard_t guard;
    esp_err_t err = http_guard_init(&guard, url, false);
    if (err != ESP_OK) {
        return err;
    }
    esp_http_client_handle_t client = NULL;
    err = http_open(&client, url, timeout_ms);
    if (err != ESP_OK) {
        http_guard_done(&guard);
        return err;
    }
    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    char *buffer = jetkvm_memory_policy_control_alloc(OTA_METADATA_BUFFER_BYTES);
    esp_err_t result = ESP_ERR_NO_MEM;
    if (buffer != NULL) {
        if (status != 200) {
            result = status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        } else if (content_length >= (int64_t)OTA_METADATA_BUFFER_BYTES) {
            result = ESP_ERR_INVALID_SIZE;
        } else {
            size_t received = 0U;
            result = ESP_OK;
            while (received < OTA_METADATA_BUFFER_BYTES - 1U) {
                int chunk = esp_http_client_read(
                    client, buffer + received,
                    OTA_METADATA_BUFFER_BYTES - 1U - received);
                if (chunk < 0) {
                    result = ESP_FAIL;
                    break;
                }
                if (chunk == 0) {
                    break;
                }
                received += (size_t)chunk;
            }
            if (result == ESP_OK) {
                buffer[received] = '\0';
                if (!jetkvm_ota_parse_metadata(buffer, received, metadata)) {
                    result = ESP_ERR_INVALID_RESPONSE;
                }
            }
        }
        jetkvm_memory_policy_control_free(buffer);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    http_guard_done(&guard);
    return result;
}

/* The ctx of http_get_small, or NULL: a request is not made, and a read
 * ends the request, once stopped() is true. */
typedef struct {
    bool (*stopped)(void);
} http_get_small_ctx_t;

static bool get_stopped(const http_get_small_ctx_t *get)
{
    return get != NULL && get->stopped != NULL && get->stopped();
}

/* GET of a small file: a signature or a key (jetkvm_ota_http_get_fn). */
static int http_get_small(void *ctx, const char *url, uint8_t *body,
                          size_t capacity, size_t *length)
{
    const http_get_small_ctx_t *get = ctx;
    *length = 0U;
    if (get_stopped(get)) {
        return -1;
    }
    ota_http_guard_t guard;
    if (http_guard_init(&guard, url, false) != ESP_OK) {
        return -1;
    }
    esp_http_client_handle_t client = NULL;
    if (http_open(&client, url, OTA_HTTP_TIMEOUT_MS) != ESP_OK) {
        http_guard_done(&guard);
        return -1;
    }
    int status = esp_http_client_get_status_code(client);
    while (status == 200) {
        if (get_stopped(get)) {
            status = -1;
            break;
        }
        if (*length == capacity) {
            /* One octet more than the file may have. */
            char extra;
            if (esp_http_client_read(client, &extra, 1) != 0) {
                status = -1;
            }
            break;
        }
        int got = esp_http_client_read(client, (char *)body + *length,
                                       capacity - *length);
        if (got < 0) {
            status = -1;
        }
        if (got <= 0) {
            break;
        }
        *length += (size_t)got;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    http_guard_done(&guard);
    return status;
}

/* Seconds since 1970, or 0 while the clock is not set: a key's and a
 * signature's expiry are checked only against a known time. */
static uint32_t signature_time(void)
{
    return jetkvm_time_sync_ready() ? (uint32_t)time(NULL) : 0U;
}

static const char *const KEYSERVERS[] = {
    JETKVM_OTA_KEYSERVER_OPENPGP,
    JETKVM_OTA_KEYSERVER_UBUNTU,
};
#define KEYSERVER_COUNT (sizeof(KEYSERVERS) / sizeof(KEYSERVERS[0]))

/* The signing keys of the release key with this fingerprint into keys, from
 * s_ota.release_keys, which the keyservers fill at most once in 24 hours.
 * The buffer for a keyserver's answer is taken only for a fetch. */
static const char *copy_release_keys(const char *fingerprint,
                                     const char *const *keyservers,
                                     size_t keyserver_count,
                                     bool (*stopped)(void),
                                     osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX],
                                     size_t *count,
                                     char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    *count = 0U;
    if (s_ota.keys_lock == NULL) {
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "failed to fetch public key: the update service is not running");
        return error;
    }
    xSemaphoreTake(s_ota.keys_lock, portMAX_DELAY);
    const int64_t uptime_seconds = esp_timer_get_time() / 1000000LL;
    uint8_t *scratch = NULL;
    const char *reason = NULL;
    if (!jetkvm_ota_release_keys_fresh(&s_ota.release_keys, fingerprint,
                                       uptime_seconds) &&
        (scratch = jetkvm_memory_policy_control_alloc(JETKVM_OTA_KEY_TEXT_MAX)) == NULL) {
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "failed to fetch public key: not enough free memory");
        reason = error;
    }
    if (reason == NULL) {
        http_get_small_ctx_t get = {stopped};
        reason = jetkvm_ota_release_keys(
            &s_ota.release_keys, fingerprint, keyservers, keyserver_count,
            http_get_small, &get, &osvc_psa_hash_ops, uptime_seconds,
            signature_time(), scratch, error);
    }
    if (reason == NULL) {
        *count = s_ota.release_keys.count;
        memcpy(keys, s_ota.release_keys.keys, *count * sizeof(keys[0]));
    }
    xSemaphoreGive(s_ota.keys_lock);
    if (scratch != NULL) {
        jetkvm_memory_policy_control_free(scratch);
    }
    return reason;
}

const char *jetkvm_ota_release_signing_keys(
    bool (*stopped)(void), osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX],
    size_t *count, char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    return copy_release_keys(JETKVM_OTA_RELEASE_KEY_FINGERPRINT, KEYSERVERS,
                             KEYSERVER_COUNT, stopped, keys, count, error);
}

/* The signing keys of the release key into s_ota.signing_keys, for the
 * image that the OTA task installs. */
static const char *fetch_release_keys(const char *update_api_url,
                                      char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    const char *fingerprint = JETKVM_OTA_RELEASE_KEY_FINGERPRINT;
    const char *const *keyservers = KEYSERVERS;
    size_t keyserver_count = KEYSERVER_COUNT;
#if JETKVM_MOCK_CLOUD
    char stand_in[JETKVM_OTA_API_URL_CAPACITY + 16U];
    const char *stand_in_list[1] = {stand_in};
    if (strcmp(update_api_url, JETKVM_OTA_DEFAULT_API_URL) != 0 &&
        strchr(update_api_url, '%') == NULL) {
        (void)snprintf(stand_in, sizeof(stand_in), "%s/keyserver/%%s",
                       update_api_url);
        fingerprint = JETKVM_OTA_TEST_RELEASE_KEY_FINGERPRINT;
        keyservers = stand_in_list;
        keyserver_count = 1U;
    }
#else
    (void)update_api_url;
#endif
    return copy_release_keys(fingerprint, keyservers, keyserver_count, NULL,
                             s_ota.signing_keys, &s_ota.signing_key_count, error);
}

/* ---- component installers (run on the OTA task) ---- */

static void mark_download_progress(float *last_pushed, float progress)
{
    float floored = progress >= 1.0f ? 1.0f :
        (float)((int)(progress * OTA_PROGRESS_PUSH_DIVISOR)) /
            (float)OTA_PROGRESS_PUSH_DIVISOR;
    if (floored <= *last_pushed && progress < 1.0f) {
        return;
    }
    *last_pushed = floored;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.state.system_download_progress = floored;
    /* Each chunk goes straight into the slot, so the download is the
     * install; reporting it as update progress too keeps the frontend's
     * system bar (weighted for a separate flash step) tracking the bytes. */
    s_ota.state.system_update_progress = floored;
    xSemaphoreGive(s_ota.lock);
    notify_state_listener();
}

/* signature: the image's detached signature, or NULL for an update that
 * needs none. A failed check writes its reason into check_error and returns
 * OTA_ERR_REPORTED. */
static esp_err_t install_system(const char *url, const char *expected_hash,
                                const uint8_t *signature, size_t signature_length,
                                char check_error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "installing firmware image into '%s' at 0x%" PRIx32,
             target->label, target->address);

    /* Before the image is asked for: a signature that none of the release
     * key's signing keys made ends the update here. */
    jetkvm_ota_image_check_t check;
    if (jetkvm_ota_image_check_begin(&check, &osvc_psa_hash_ops, signature,
                                     signature_length, s_ota.signing_keys,
                                     s_ota.signing_key_count,
                                     check_error) != NULL) {
        return OTA_ERR_REPORTED;
    }
    ota_http_guard_t guard;
    esp_err_t err = http_guard_init(&guard, url, true);
    if (err != ESP_OK) {
        jetkvm_ota_image_check_abort(&check);
        return err;
    }
    esp_http_client_handle_t client = NULL;
    err = http_open(&client, url, OTA_HTTP_TIMEOUT_MS);
    if (err != ESP_OK) {
        jetkvm_ota_image_check_abort(&check);
        http_guard_done(&guard);
        return err;
    }
    esp_err_t result = ESP_OK;
    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    uint8_t *chunk = NULL;
    size_t chunk_size = OTA_DOWNLOAD_CHUNK_BYTES;
    esp_ota_handle_t handle = 0;
    bool begun = false;
    if (status != 200) {
        result = ESP_FAIL;
        goto done;
    }
    if (content_length <= 0 || (uint64_t)content_length > target->size) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    chunk = jetkvm_memory_policy_control_alloc(chunk_size);
    if (chunk == NULL) {
        chunk_size = OTA_DOWNLOAD_CHUNK_FALLBACK_BYTES;
        chunk = jetkvm_memory_policy_control_alloc(chunk_size);
    }
    if (chunk == NULL) {
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    /* Sequential-writes mode erases each block as the download reaches it
     * instead of stalling the whole system on one up-front multi-megabyte
     * erase; the load (and the flash activity before the final reboot) is
     * spread across the entire download. */
    if ((result = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES,
                                &handle)) != ESP_OK) {
        goto done;
    }
    begun = true;
    {
        size_t written = 0U;
        float last_pushed = 0.0f;
        uint8_t head[OTA_IMAGE_HEAD_BYTES];
        size_t filled = 0U;
        while (written < (size_t)content_length) {
            size_t block = (size_t)content_length - written;
            if (block > chunk_size) {
                block = chunk_size;
            }
            int got = esp_http_client_read(client, (char *)chunk + filled,
                                           block - filled);
            if (got < 0) {
                result = ESP_FAIL;
                goto done;
            }
            if (got == 0) {
                result = ESP_ERR_INVALID_SIZE;
                goto done;
            }
            size_t received = written + filled;
            if (received < OTA_IMAGE_HEAD_BYTES) {
                size_t take = OTA_IMAGE_HEAD_BYTES - received;
                if (take > (size_t)got) {
                    take = (size_t)got;
                }
                memcpy(head + received, chunk + filled, take);
                if (received + take == OTA_IMAGE_HEAD_BYTES) {
                    jetkvm_ota_image_desc_t desc;
                    memcpy(&desc, head + OTA_IMAGE_DESC_OFFSET, sizeof(desc));
                    if (!jetkvm_ota_image_desc_matches(&desc, JETKVM_DEVICE_SKU)) {
                        if (desc.magic == JETKVM_OTA_IMAGE_DESC_MAGIC) {
                            ESP_LOGE(TAG, "image is for %.*s, this board is "
                                     JETKVM_DEVICE_SKU,
                                     (int)sizeof(desc.sku), desc.sku);
                        } else {
                            ESP_LOGE(TAG, "image has no SKU marker, this "
                                     "board is " JETKVM_DEVICE_SKU);
                        }
                        result = ESP_ERR_INVALID_VERSION;
                        goto done;
                    }
                }
            }
            if (!jetkvm_ota_image_check_update(&check, chunk + filled,
                                               (size_t)got)) {
                result = ESP_FAIL;
                goto done;
            }
            filled += (size_t)got;
            if (filled < block) {
                continue;
            }
            result = esp_ota_write(handle, chunk, filled);
            if (result != ESP_OK) {
                goto done;
            }
            written += filled;
            filled = 0U;
            mark_download_progress(&last_pushed,
                                   (float)written / (float)content_length);
        }
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.state.system_download_finished = true;
    xSemaphoreGive(s_ota.lock);
    notify_state_listener();
    /* The image is in the slot and not yet the boot image: esp_ota_end and
     * the boot partition follow only a hash and a signature that hold. */
    if (jetkvm_ota_image_check_end(&check, expected_hash, signature_time(),
                                   check_error) != NULL) {
        result = OTA_ERR_REPORTED;
        goto done;
    }
    if (signature != NULL) {
        ESP_LOGI(TAG, "GPG signature verified");
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.state.system_verification_progress = 1.0f;
    s_ota.state.system_verified = true;
    xSemaphoreGive(s_ota.lock);
    notify_state_listener();
    result = esp_ota_end(handle);
    begun = false;
    if (result != ESP_OK) {
        goto done;
    }
    result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        goto done;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.state.system_update_progress = 1.0f;
    s_ota.state.system_updated = true;
    xSemaphoreGive(s_ota.lock);
    notify_state_listener();
done:
    jetkvm_ota_image_check_abort(&check);
    if (begun) {
        (void)esp_ota_abort(handle);
    }
    if (chunk != NULL) {
        jetkvm_memory_policy_control_free(chunk);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    http_guard_done(&guard);
    return result;
}

/* ---- update task ---- */

/* A pinned install keeps automatic updates off through the erase: without a
 * record the defaults turn them on, and the next check would replace the
 * pinned version. NVS is closed again after that one record, so no other
 * setting is written before the reboot. */
static void erase_settings_for_reset_config(bool pinned)
{
    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if (nvs == NULL) {
        ESP_LOGE(TAG, "resetConfig requested but the nvs partition is missing");
        return;
    }
    ESP_LOGW(TAG, "resetConfig: erasing all persisted settings");
    (void)nvs_flash_deinit();
    esp_err_t err = esp_partition_erase_range(nvs, 0, nvs->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings erase failed: %s", esp_err_to_name(err));
        return;
    }
    if (!pinned) {
        return;
    }
    jetkvm_ota_config_t config;
    config_defaults(&config);
    config.auto_update_enabled = false;
    err = nvs_flash_init();
    if (err == ESP_OK) {
        err = config_save(&config);
        (void)nvs_flash_deinit();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "automatic updates may be on after the reset: %s",
                 esp_err_to_name(err));
    }
}

/* The install's turn: a side download stops first. */
static bool begin_install(void)
{
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.installing = true;
    void (*stop)(void *ctx) = s_ota.side_active ? s_ota.side_stop : NULL;
    void *ctx = s_ota.side_ctx;
    xSemaphoreGive(s_ota.lock);
    if (stop != NULL) {
        ESP_LOGI(TAG, "stopping the OS Services update for the install");
        stop(ctx);
    }
    for (int waited = 0; __atomic_load_n(&s_ota.side_active, __ATOMIC_ACQUIRE) &&
                         waited < OTA_SIDE_STOP_WAIT_MS;
         waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !__atomic_load_n(&s_ota.side_active, __ATOMIC_ACQUIRE);
}

static void run_update(const ota_request_t *request)
{
    jetkvm_ota_config_t config;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    config = s_ota.config;
    memset(&s_ota.state, 0, sizeof(s_ota.state));
    s_ota.state.updating = true;
    xSemaphoreGive(s_ota.lock);
    notify_state_listener();

    if (!jetkvm_ota_validate_api_url(config.update_api_url)) {
        set_error("no update server is configured");
        return;
    }
    char device_id[32] = {0};
    if (jetkvm_device_state_get_id(device_id, sizeof(device_id)) != ESP_OK) {
        set_error("device identity unavailable");
        return;
    }
    char url[JETKVM_OTA_RELEASES_URL_CAPACITY];
    if (!jetkvm_ota_build_releases_url(
            url, sizeof(url), config.update_api_url, device_id,
            JETKVM_DEVICE_SKU, request->include_prerelease,
            request->app_constraint, request->system_constraint)) {
        set_error("update server URL is too long");
        return;
    }
    jetkvm_ota_metadata_t metadata;
    esp_err_t err = fetch_metadata(url, OTA_HTTP_TIMEOUT_MS, &metadata);
    if (err == ESP_ERR_NOT_FOUND) {
        set_error("requested version was not found on the update server");
        return;
    }
    if (err == OTA_ERR_TLS_ADMISSION) {
        set_error(OTA_ERROR_TLS_ADMISSION);
        return;
    }
    if (err == ESP_ERR_NO_MEM) {
        set_error(OTA_ERROR_NO_MEMORY_CHECK);
        return;
    }
    if (err != ESP_OK) {
        set_error("could not reach the update server");
        return;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.state.metadata_fetched = true;
    s_ota.status.remote = metadata;
    jetkvm_ota_local_versions(s_ota.status.local_app_version,
                   sizeof(s_ota.status.local_app_version),
                   s_ota.status.local_system_version,
                   sizeof(s_ota.status.local_system_version));
    compute_availability(&s_ota.status, request->system_constraint);
    s_ota.status.error[0] = '\0';
    s_ota.status_valid = true;
    bool want_system = s_ota.status.system_update_available;
    s_ota.state.system_update_pending = want_system;
    if (metadata.app_version[0] != '\0') {
        ESP_LOGW(TAG, "release offers an app component (%s); this product has "
                 "no app artifact, ignoring it", metadata.app_version);
    }
    /* Custom-version installs opt out of the hourly channel, exactly like
     * Go's willDisableAutoUpdate behaviour after a pinned update. */
    s_ota.status.will_disable_auto_update =
        (request->app_constraint[0] != '\0' ||
         request->system_constraint[0] != '\0') && config.auto_update_enabled;
    xSemaphoreGive(s_ota.lock);
    notify_state_listener();

    if (!want_system) {
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        s_ota.state.updating = false;
        xSemaphoreGive(s_ota.lock);
        notify_state_listener();
        return;
    }
    if (!begin_install()) {
        set_error("the OS Services update did not stop; try again");
        return;
    }
    if (s_ota.hooks.video_pause) {
        s_ota.hooks.video_pause(true, s_ota.hooks.video_pause_ctx);
    }
    /* The signature and the key come before the image, one connection
     * after the other: the image is hashed for the signature while it
     * streams into the slot, and an update that cannot be checked ends
     * before it writes to flash. */
    char error[JETKVM_OTA_ERROR_CAPACITY] = {0};
    char reason[JETKVM_OTA_SIGNATURE_ERROR_MAX];
    uint8_t *signature = NULL;
    size_t signature_length = 0U;
    esp_err_t installed = ESP_OK;
    if (jetkvm_ota_signature_bypass(metadata.system_version,
                                    request->system_constraint[0] != '\0',
                                    request->include_prerelease)) {
        ESP_LOGW(TAG, "bypassing GPG signature check for version %s",
                 metadata.system_version);
    } else if ((signature = jetkvm_memory_policy_control_alloc(
                    JETKVM_OTA_SIGNATURE_MAX)) == NULL) {
        installed = ESP_ERR_NO_MEM;
    } else if (jetkvm_ota_signature_download(
                   http_get_small, NULL, metadata.system_version,
                   metadata.system_sig_url, signature, JETKVM_OTA_SIGNATURE_MAX,
                   &signature_length, reason) != NULL) {
        (void)snprintf(error, sizeof(error), OTA_ERROR_SIGNATURE "%s", reason);
        installed = OTA_ERR_REPORTED;
    } else if (fetch_release_keys(config.update_api_url, reason) != NULL) {
        (void)snprintf(error, sizeof(error),
                       OTA_ERROR_VERIFY "GPG signature verification failed: %s",
                       reason);
        installed = OTA_ERR_REPORTED;
    }
    if (installed == ESP_OK) {
        installed = install_system(metadata.system_url, metadata.system_hash,
                                   signature, signature_length, reason);
        if (installed == OTA_ERR_REPORTED) {
            (void)snprintf(error, sizeof(error), OTA_ERROR_VERIFY "%s", reason);
        }
    }
    if (signature != NULL) {
        jetkvm_memory_policy_control_free(signature);
    }
    if (installed != ESP_OK) {
        if (s_ota.hooks.video_pause) {
            s_ota.hooks.video_pause(false, s_ota.hooks.video_pause_ctx);
        }
        set_error(installed == OTA_ERR_REPORTED ? error :
                  installed == ESP_ERR_INVALID_VERSION ?
                      "firmware image is built for a different board" :
                  installed == OTA_ERR_TLS_ADMISSION ? OTA_ERROR_TLS_ADMISSION :
                  installed == ESP_ERR_NO_MEM ? OTA_ERROR_NO_MEMORY_DOWNLOAD :
                      "firmware image download or install failed");
        return;
    }
    if (s_ota.status.will_disable_auto_update) {
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        s_ota.config.auto_update_enabled = false;
        esp_err_t saved = config_save(&s_ota.config);
        xSemaphoreGive(s_ota.lock);
        if (saved != ESP_OK) {
            ESP_LOGW(TAG, "auto-update opt-out was not persisted: %s",
                     esp_err_to_name(saved));
        }
    }
    /* `updating` stays true through the reboot, as in Go: the UI reloads the
     * page as soon as it sees updating=false without an error, and a reload
     * before willReboot lands on the old web pack instead of the rebooting
     * overlay that polls the health check. */
    ESP_LOGW(TAG, "update installed; rebooting%s",
             request->reset_config ? " with settings reset" : "");
    if (request->reset_config) {
        erase_settings_for_reset_config(request->app_constraint[0] != '\0' ||
                                        request->system_constraint[0] != '\0');
    }
    if (s_ota.hooks.reboot) {
        s_ota.hooks.reboot("/settings/general/update", s_ota.hooks.reboot_ctx);
    }
}

static bool auto_update_due(void)
{
    if (!s_ota.config.auto_update_enabled || s_ota.state.updating) {
        return false;
    }
    if (esp_timer_get_time() < s_ota.next_auto_update_us) {
        return false;
    }
    if (!jetkvm_time_sync_ready()) {
        return false;
    }
    return !jetkvm_ota_session_active();
}

static void ota_task(void *arg)
{
    (void)arg;
    s_ota.next_auto_update_us =
        esp_timer_get_time() + OTA_AUTO_UPDATE_FIRST_DELAY_US;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        if (s_ota.stopping) {
            xSemaphoreGive(s_ota.lock);
            break;
        }
        ota_request_t request = s_ota.request;
        bool has_request = request.pending;
        if (has_request) {
            s_ota.request.pending = false;
        }
        bool auto_due = !has_request && auto_update_due();
        if (auto_due) {
            s_ota.next_auto_update_us =
                esp_timer_get_time() + OTA_AUTO_UPDATE_INTERVAL_US;
            memset(&request, 0, sizeof(request));
        }
        /* The update is in progress from here on. run_update sets the same
         * flag, but only when it has taken the lock again: a call of
         * jetkvm_ota_is_update_pending() that waited for the lock would run
         * in between and see neither the request nor the update. */
        if (has_request || auto_due) {
            s_ota.state.updating = true;
        }
        xSemaphoreGive(s_ota.lock);
        if (has_request) {
            run_update(&request);
        } else if (auto_due) {
            ESP_LOGI(TAG, "running scheduled auto-update check");
            run_update(&request);
        }
    }
    vTaskDelete(NULL);
}

/* ---- public API ---- */

esp_err_t jetkvm_ota_start(const jetkvm_ota_hooks_t *hooks)
{
    if (s_ota.lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_ota, 0, sizeof(s_ota));
    if (hooks != NULL) {
        s_ota.hooks = *hooks;
    }
    s_ota.keys_lock = xSemaphoreCreateMutex();
    if (s_ota.keys_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_ota.lock = xSemaphoreCreateMutex();
    if (s_ota.lock == NULL) {
        vSemaphoreDelete(s_ota.keys_lock);
        s_ota.keys_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = config_load(&s_ota.config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA config load failed (%s); using defaults",
                 esp_err_to_name(err));
        config_defaults(&s_ota.config);
    }
    jetkvm_ota_local_versions(s_ota.status.local_app_version,
                   sizeof(s_ota.status.local_app_version),
                   s_ota.status.local_system_version,
                   sizeof(s_ota.status.local_system_version));
    if (xTaskCreatePinnedToCore(ota_task, "jetkvm_ota", OTA_TASK_STACK_BYTES,
                                NULL, OTA_TASK_PRIORITY, &s_ota.task,
                                0) != pdPASS) {
        vSemaphoreDelete(s_ota.lock);
        s_ota.lock = NULL;
        vSemaphoreDelete(s_ota.keys_lock);
        s_ota.keys_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void jetkvm_ota_stop(void)
{
    if (s_ota.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.stopping = true;
    TaskHandle_t task = s_ota.task;
    xSemaphoreGive(s_ota.lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

esp_err_t jetkvm_ota_get_config(jetkvm_ota_config_t *config)
{
    if (config == NULL || s_ota.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    *config = s_ota.config;
    xSemaphoreGive(s_ota.lock);
    return ESP_OK;
}

static esp_err_t persist_config(void)
{
    esp_err_t saved = config_save(&s_ota.config);
    if (saved != ESP_OK) {
        ESP_LOGE(TAG, "OTA config save failed: %s", esp_err_to_name(saved));
    }
    return saved;
}

esp_err_t jetkvm_ota_set_update_api_url(const char *url)
{
    if (s_ota.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (url[0] == '\0') {
        url = JETKVM_OTA_DEFAULT_API_URL;
    } else if (!jetkvm_ota_validate_api_url(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    (void)snprintf(s_ota.config.update_api_url,
                   sizeof(s_ota.config.update_api_url), "%s", url);
    esp_err_t saved = persist_config();
    xSemaphoreGive(s_ota.lock);
    return saved;
}

esp_err_t jetkvm_ota_set_auto_update(bool enabled)
{
    if (s_ota.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.config.auto_update_enabled = enabled;
    s_ota.next_auto_update_us =
        esp_timer_get_time() + OTA_AUTO_UPDATE_INTERVAL_US;
    esp_err_t saved = persist_config();
    xSemaphoreGive(s_ota.lock);
    return saved;
}

esp_err_t jetkvm_ota_set_include_prerelease(bool enabled)
{
    if (s_ota.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.config.include_prerelease = enabled;
    esp_err_t saved = persist_config();
    xSemaphoreGive(s_ota.lock);
    return saved;
}

esp_err_t jetkvm_ota_get_update_status(jetkvm_ota_update_status_t *status)
{
    if (status == NULL || s_ota.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    *status = s_ota.status;
    jetkvm_ota_local_versions(status->local_app_version,
                   sizeof(status->local_app_version),
                   status->local_system_version,
                   sizeof(status->local_system_version));
    /* the cached metadata came from the last plain check, so the
     * unconstrained comparison applies */
    compute_availability(status, NULL);
    xSemaphoreGive(s_ota.lock);
    return ESP_OK;
}

esp_err_t jetkvm_ota_check_update(const char *app_constraint,
                                  const char *system_constraint,
                                  bool include_prerelease,
                                  jetkvm_ota_update_status_t *status)
{
    if (status == NULL || s_ota.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    jetkvm_ota_config_t config;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    config = s_ota.config;
    bool updating = s_ota.state.updating;
    xSemaphoreGive(s_ota.lock);
    if (updating) {
        /* Go serves the last known status while an install runs; fetching
         * again would only make the caller wait. */
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        *status = s_ota.status;
        jetkvm_ota_local_versions(status->local_app_version,
                       sizeof(status->local_app_version),
                       status->local_system_version,
                       sizeof(status->local_system_version));
        xSemaphoreGive(s_ota.lock);
        return ESP_OK;
    }
    if (!jetkvm_ota_validate_api_url(config.update_api_url)) {
        memset(status, 0, sizeof(*status));
        jetkvm_ota_local_versions(status->local_app_version,
                       sizeof(status->local_app_version),
                       status->local_system_version,
                       sizeof(status->local_system_version));
        (void)snprintf(status->error, sizeof(status->error),
                       "no update server is configured");
        return ESP_OK;
    }
    char device_id[32] = {0};
    if (jetkvm_device_state_get_id(device_id, sizeof(device_id)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    char url[JETKVM_OTA_RELEASES_URL_CAPACITY];
    if (!jetkvm_ota_build_releases_url(url, sizeof(url),
                                       config.update_api_url, device_id,
                                       JETKVM_DEVICE_SKU, include_prerelease,
                                       app_constraint, system_constraint)) {
        return ESP_ERR_INVALID_SIZE;
    }
    jetkvm_ota_metadata_t metadata;
    esp_err_t err = fetch_metadata(url, OTA_CHECK_TIMEOUT_MS, &metadata);
    memset(status, 0, sizeof(*status));
    jetkvm_ota_local_versions(status->local_app_version,
                   sizeof(status->local_app_version),
                   status->local_system_version,
                   sizeof(status->local_system_version));
    if (err == ESP_OK) {
        status->remote = metadata;
        compute_availability(status, system_constraint);
        /* An install that was requested while this check waited for the
         * server owns the shared status: its willDisableAutoUpdate decides
         * the auto-update setting after the install. */
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        if (!s_ota.state.updating && !s_ota.request.pending) {
            s_ota.status = *status;
            s_ota.status_valid = true;
        }
        xSemaphoreGive(s_ota.lock);
    } else if (err == ESP_ERR_NOT_FOUND) {
        (void)snprintf(status->error, sizeof(status->error),
                       "requested version was not found on the update server");
    } else if (err == OTA_ERR_TLS_ADMISSION) {
        (void)snprintf(status->error, sizeof(status->error), "%s",
                       OTA_ERROR_TLS_ADMISSION);
    } else if (err == ESP_ERR_NO_MEM) {
        (void)snprintf(status->error, sizeof(status->error), "%s",
                       OTA_ERROR_NO_MEMORY_CHECK);
    } else {
        (void)snprintf(status->error, sizeof(status->error),
                       "could not reach the update server");
    }
    return ESP_OK;
}

esp_err_t jetkvm_ota_try_update(const char *app_constraint,
                                const char *system_constraint,
                                bool include_prerelease,
                                bool reset_config)
{
    if (s_ota.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    if (s_ota.state.updating || s_ota.request.pending) {
        xSemaphoreGive(s_ota.lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_ota.request, 0, sizeof(s_ota.request));
    if (app_constraint != NULL) {
        (void)snprintf(s_ota.request.app_constraint,
                       sizeof(s_ota.request.app_constraint), "%s",
                       app_constraint);
    }
    if (system_constraint != NULL) {
        (void)snprintf(s_ota.request.system_constraint,
                       sizeof(s_ota.request.system_constraint), "%s",
                       system_constraint);
    }
    s_ota.request.include_prerelease = include_prerelease;
    s_ota.request.reset_config = reset_config;
    s_ota.request.pending = true;
    TaskHandle_t task = s_ota.task;
    xSemaphoreGive(s_ota.lock);
    xTaskNotifyGive(task);
    return ESP_OK;
}

bool jetkvm_ota_side_download_begin(void (*stop)(void *ctx), void *ctx)
{
    if (s_ota.lock == NULL) {
        return true;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    const bool admitted = !s_ota.installing && !s_ota.side_active;
    if (admitted) {
        s_ota.side_stop = stop;
        s_ota.side_ctx = ctx;
        __atomic_store_n(&s_ota.side_active, true, __ATOMIC_RELEASE);
    }
    xSemaphoreGive(s_ota.lock);
    return admitted;
}

void jetkvm_ota_side_download_end(void)
{
    if (s_ota.lock != NULL) {
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        __atomic_store_n(&s_ota.side_active, false, __ATOMIC_RELEASE);
        xSemaphoreGive(s_ota.lock);
    }
}

bool jetkvm_ota_session_active(void)
{
    return s_ota.hooks.session_active != NULL &&
           s_ota.hooks.session_active(s_ota.hooks.session_active_ctx);
}

bool jetkvm_ota_is_update_pending(void)
{
    if (s_ota.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    bool pending = s_ota.state.updating || s_ota.request.pending;
    xSemaphoreGive(s_ota.lock);
    return pending;
}

void jetkvm_ota_snapshot_state(jetkvm_ota_state_t *state)
{
    if (state == NULL || s_ota.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    *state = s_ota.state;
    xSemaphoreGive(s_ota.lock);
}

void jetkvm_ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL ||
        (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        return;
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        ESP_LOGW(TAG, "could not read OTA state for '%s'", running->label);
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "running OTA image '%s' confirmed valid: %s",
                 running->label, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "running OTA image '%s' already in state %d",
                 running->label, (int)state);
    }
}

bool jetkvm_ota_render_current_state_event(char *output, size_t capacity,
                                           size_t *out_size)
{
    jetkvm_ota_state_t state;
    jetkvm_ota_snapshot_state(&state);
    char timestamp[JETKVM_OTA_TIMESTAMP_CAPACITY];
    current_timestamp(timestamp, sizeof(timestamp));
    return jetkvm_ota_render_state_event(&state, timestamp, output, capacity,
                                         out_size);
}
