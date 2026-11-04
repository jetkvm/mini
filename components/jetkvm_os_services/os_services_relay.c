/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The update relay's device side: the worker task's HTTPS, the buffers it
 * hands to the OS Services task, and the state the browser reads. The
 * checks themselves are in os_services_update.c and os_services_release.c.
 */

#include "os_services_relay.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_ota.h"
#include "jetkvm_ota_signature.h"
#include "os_services_pgp_psa.h"
#include "os_services_release.h"
#include "os_services_update.h"
#include "psa/crypto.h"

static const char *TAG = "osvc_update";

/* The release bucket. A bench build that tests against the e2e mock cloud
 * (JETKVM_MOCK_CLOUD) takes <update API URL>/os-services instead. */
#define BUCKET_URL "https://os-services.jetkvm.com"
#ifndef JETKVM_MOCK_CLOUD
#define JETKVM_MOCK_CLOUD 0
#endif
#define URL_CAPACITY (JETKVM_OTA_API_URL_CAPACITY + OSVC_RELEASE_PATH_MAX + 24U)
#define ETAG_CAPACITY 96U
#define INDEX_MAX (256U * 1024U)
#define SIGNATURE_MAX 4096U
#define BUFFER_BYTES (64U * 1024U)
/* esp_http_client_read() returns only once it has all it was asked for, so
 * a firmware install's stop is seen after at most this many bytes. */
#define READ_STEP_BYTES 4096U
/* HTTPS and Monocypher's Ed25519 (2 KiB); in PSRAM, as no flash work runs
 * on it. */
#define WORKER_STACK_BYTES 10240U
/* Below the OS Services task (4), which relays the buffers. */
#define WORKER_PRIORITY 3U
#define SECOND_US 1000000LL
#define CHECK_INTERVAL_US (300LL * SECOND_US)
/* A check that did not get the indexes is tried again after this, then
 * twice as long each time up to the check interval. */
#define FIRST_RETRY_US (15LL * SECOND_US)
#define INDEX_MAX_AGE_US (3600LL * SECOND_US)
#define CONNECT_MAX_AGE_US (600LL * SECOND_US)
#define AUTO_RETRY_US (3600LL * SECOND_US)
#define RESTART_TIMEOUT_US (180LL * SECOND_US)
#define UNAVAILABLE "the OS Services sender cannot install an update"
#define STOPPED "stopped for a firmware update"
#define FIRMWARE_UPDATING "a firmware update is in progress"

enum { STABLE = 0, DEV = 1 };

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t service_task;
    TaskHandle_t worker;
    /* The worker's jobs. */
    bool refresh_wanted;
    int64_t refresh_max_age_us;
    bool update_wanted;
    /* The OTA settings, as the service task last read them. */
    bool automatic;
    bool include_dev;
    /* The indexes' newest releases, their ETags, and when they were read. */
    osvc_release_t releases[2];
    char etags[2][ETAG_CAPACITY];
    int64_t fetched_us;
    bool dev_fetched;
    int64_t next_check_us;
    int64_t retry_us;
    /* The worker's parse target, kept off its stack. */
    osvc_release_t scratch;
    /* The worker's copy of the release key's signing keys, for the index or
     * the package that it checks, and why it has none. */
    osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX];
    size_t key_count;
    char key_error[JETKVM_OTA_SIGNATURE_ERROR_MAX];
    /* The connected sender, and the session it reported in. */
    osvc_update_sender_t sender;
    uint32_t sender_session;
    /* The update and the package it installs. */
    osvc_update_progress_t progress;
    osvc_release_package_t package;
    bool package_signed;
    char auto_tried[OSVC_UPDATE_VERSION_MAX + 1U];
    int64_t auto_tried_us;
    /* The sender session the package was chosen for; only it installs. */
    uint32_t update_session;
    int64_t installing_deadline_us;
    /* Between the worker and the OS Services task. */
    bool begin;
    bool cancel;
    char cancel_reason[OSVC_UPDATE_MESSAGE_CAPACITY];
    uint32_t update_id;
    const uint8_t *chunks[2];
    size_t chunk_lengths[2];
    size_t chunk_count;
    size_t chunk_sent;
    bool ended;
    uint8_t status;
    char message[OSVC_UPDATE_MESSAGE_CAPACITY];
    /* A firmware install needs the network to itself. */
    bool stop;
    uint32_t generation;
    uint32_t pushed_generation;
    int64_t progress_pushed_us;
} relay_t;

static EXT_RAM_BSS_ATTR relay_t s_relay;

static void lock(void)
{
    xSemaphoreTake(s_relay.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_relay.lock);
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* The browser should see the state again. Under the lock. */
static void changed_locked(void)
{
    s_relay.generation++;
    xTaskNotifyGive(s_relay.service_task);
}

static void notify_worker_locked(void)
{
    if (s_relay.worker != NULL) {
        xTaskNotifyGive(s_relay.worker);
    }
}

static void *psram_alloc(size_t size)
{
    jetkvm_memory_policy_reservation_t reservation = {0};
    if (!jetkvm_memory_policy_reserve_psram("osvc-update", size,
                                            &reservation)) {
        return NULL;
    }
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jetkvm_memory_policy_release(&reservation);
    return memory;
}

/* --- Hashes (PSA) -------------------------------------------------------- */

static void *hash_begin(void *ctx, osvc_pgp_hash_t algorithm)
{
    (void)ctx;
    const psa_algorithm_t psa = algorithm == OSVC_PGP_HASH_SHA1 ? PSA_ALG_SHA_1 :
                                algorithm == OSVC_PGP_HASH_SHA256 ? PSA_ALG_SHA_256 :
                                algorithm == OSVC_PGP_HASH_SHA384 ? PSA_ALG_SHA_384 :
                                                                    PSA_ALG_SHA_512;
    psa_hash_operation_t *hash =
        heap_caps_calloc(1U, sizeof(*hash), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (hash != NULL && psa_hash_setup(hash, psa) != PSA_SUCCESS) {
        heap_caps_free(hash);
        hash = NULL;
    }
    return hash;
}

static bool hash_update(void *ctx, void *hash, const uint8_t *data, size_t length)
{
    (void)ctx;
    return psa_hash_update(hash, data, length) == PSA_SUCCESS;
}

static void hash_abort(void *ctx, void *hash)
{
    (void)ctx;
    (void)psa_hash_abort(hash);
    heap_caps_free(hash);
}

static bool hash_finish(void *ctx, void *hash, uint8_t *digest, size_t capacity,
                        size_t *length)
{
    const bool ok = psa_hash_finish(hash, digest, capacity, length) == PSA_SUCCESS;
    hash_abort(ctx, hash);
    return ok;
}

const osvc_hash_ops_t osvc_psa_hash_ops = {
    .begin = hash_begin,
    .update = hash_update,
    .finish = hash_finish,
    .abort = hash_abort,
};

/* Signature expiry is checked only once the clock is plausible. */
static uint32_t signature_time(void)
{
    const time_t now = time(NULL);
    return now > 1767225600 ? (uint32_t)now : 0U;
}

/* --- HTTPS --------------------------------------------------------------- */

typedef struct {
    esp_http_client_handle_t client;
    jetkvm_memory_policy_reservation_t tls;
    char etag[ETAG_CAPACITY];
} http_t;

static esp_err_t on_http_event(esp_http_client_event_t *event)
{
    http_t *http = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(event->header_key, "ETag") == 0) {
        snprintf(http->etag, sizeof(http->etag), "%s", event->header_value);
    }
    return ESP_OK;
}

/* After a firmware install's stop no download opens or reads. */
static bool stopped(void)
{
    return __atomic_load_n(&s_relay.stop, __ATOMIC_ACQUIRE);
}

static void http_close(http_t *http)
{
    if (http->client != NULL) {
        esp_http_client_close(http->client);
        esp_http_client_cleanup(http->client);
        http->client = NULL;
    }
    jetkvm_memory_policy_release(&http->tls);
}

/* Opens a GET, as the OTA does. package is true for the package, whose
 * download also reserves the Wi-Fi link's blocks, as a firmware download
 * does. */
static const char *http_open(http_t *http, const char *url,
                             const char *if_none_match, bool package,
                             int64_t *length)
{
    memset(http, 0, sizeof(*http));
    if (stopped()) {
        return STOPPED;
    }
    if (!(package ?
            jetkvm_memory_policy_wait_reserve_download("osvc-update",
                                                       &http->tls) :
            jetkvm_memory_policy_wait_reserve_tls("osvc-update",
                                                  &http->tls))) {
        return "not enough free memory for a secure connection";
    }
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OSVC_UPDATE_IDLE_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .event_handler = on_http_event,
        .user_data = http,
    };
    http->client = esp_http_client_init(&config);
    if (http->client != NULL && if_none_match[0] != '\0') {
        esp_http_client_set_header(http->client, "If-None-Match", if_none_match);
    }
    const bool connected = http->client != NULL &&
                           esp_http_client_open(http->client, 0) == ESP_OK;
    if (!connected ||
        (*length = esp_http_client_fetch_headers(http->client)) < 0) {
        http_close(http);
        return "could not reach the OS Services release server";
    }
    return NULL;
}

/* Reads exactly length bytes; the relay's io.read. */
static bool http_read(void *ctx, uint8_t *data, size_t length)
{
    http_t *http = ctx;
    for (size_t done = 0U; done < length;) {
        const size_t step = length - done < READ_STEP_BYTES ? length - done :
                                                              READ_STEP_BYTES;
        const int n = stopped() ? -1 :
                      esp_http_client_read(http->client, (char *)data + done,
                                           (int)step);
        if (n <= 0) {
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

typedef struct {
    int status;
    /* PSRAM, for a 200 only; the caller frees it. */
    uint8_t *body;
    size_t length;
    char etag[ETAG_CAPACITY];
} response_t;

/* GETs a small file (an index or a signature) of at most limit bytes. */
static const char *fetch(const char *url, const char *if_none_match,
                         size_t limit, response_t *response)
{
    http_t http;
    int64_t length = 0;
    memset(response, 0, sizeof(*response));
    const char *error = http_open(&http, url, if_none_match, false, &length);
    if (error != NULL) {
        return error;
    }
    response->status = esp_http_client_get_status_code(http.client);
    if (response->status == 200) {
        response->length = (size_t)length;
        error = length <= 0 || (uint64_t)length > limit ?
                    "an OS Services release file of an unexpected size" :
                (response->body = psram_alloc(response->length)) == NULL ?
                    "no memory for the OS Services release index" :
                !http_read(&http, response->body, response->length) ?
                    "the OS Services release download failed" :
                    NULL;
    }
    memcpy(response->etag, http.etag, sizeof(response->etag));
    http_close(&http);
    return error;
}

/* <bucket>/<path><suffix> */
static void file_url(char url[URL_CAPACITY], const char *path, const char *suffix)
{
    jetkvm_ota_config_t config;
    const bool mock = JETKVM_MOCK_CLOUD && jetkvm_ota_get_config(&config) == ESP_OK;
    snprintf(url, URL_CAPACITY, "%s%s/%s%s",
             mock ? config.update_api_url : BUCKET_URL,
             mock ? "/os-services" : "", path, suffix);
}

/* The signing keys of the JetKVM release key into s_relay.keys, from the
 * keyservers through the firmware update's copy. Returns NULL, or a reason
 * short enough for the browser's error line, which refuses the index or the
 * package; the log has the keyservers' reason. A firmware install's stop
 * ends the fetch after the request in progress. */
static const char *release_keys(void)
{
    const char *error = jetkvm_ota_release_signing_keys(
        stopped, s_relay.keys, &s_relay.key_count, s_relay.key_error);
    if (error != NULL && stopped()) {
        return STOPPED;
    }
    if (error != NULL) {
        ESP_LOGW(TAG, "%s", error);
        return "the JetKVM release key is unavailable";
    }
    return NULL;
}

/* --- Release indexes ----------------------------------------------------- */

/* Fetches one index into its cached copy. Nothing changes on failure. */
static const char *fetch_index(int which)
{
    static const char *const NAMES[2] = {"releases.json", "releases-dev.json"};
    char url[URL_CAPACITY];
    char etag[ETAG_CAPACITY];
    file_url(url, NAMES[which], "");
    lock();
    memcpy(etag, s_relay.etags[which], sizeof(etag));
    unlock();
    response_t index;
    response_t signature = {0};
    const char *error = fetch(url, etag, INDEX_MAX, &index);
    const bool fresh = error == NULL && index.status == 200;
    if (fresh && which == STABLE) {
        file_url(url, NAMES[which], ".sig");
        error = fetch(url, "", SIGNATURE_MAX, &signature);
        error = error != NULL ? error :
                signature.status != 200 ?
                    "the OS Services release index has no signature" :
                    release_keys();
        error = error != NULL ? error :
                osvc_pgp_verify_detached(signature.body, signature.length,
                                         index.body, index.length, s_relay.keys,
                                         s_relay.key_count, &osvc_psa_hash_ops,
                                         signature_time());
    }
    if (fresh && error == NULL) {
        error = osvc_release_read_index((const char *)index.body, index.length,
                                        which == STABLE, &s_relay.scratch);
    }
    /* A dev index that does not exist lists no releases. */
    const bool gone = error == NULL && index.status == 404 && which == DEV;
    if (error == NULL && !fresh && !gone && index.status != 304) {
        error = "the OS Services release index is unavailable";
    }
    if (error == NULL && index.status != 304) {
        lock();
        if (fresh) {
            s_relay.releases[which] = s_relay.scratch;
        } else {
            memset(&s_relay.releases[which], 0, sizeof(s_relay.releases[which]));
        }
        snprintf(s_relay.etags[which], ETAG_CAPACITY, "%s",
                 fresh ? index.etag : "");
        changed_locked();
        unlock();
    }
    heap_caps_free(index.body);
    heap_caps_free(signature.body);
    return error;
}

/* No check has got the indexes yet, as at boot before the network is up:
 * nothing is offered until one does, so try again soon. An index already
 * read is asked for with its ETag, so the bucket sends it again only if it
 * changed. */
static void retry_locked(void)
{
    if (s_relay.fetched_us != 0) {
        return;
    }
    s_relay.retry_us = s_relay.retry_us == 0 ? FIRST_RETRY_US : 2 * s_relay.retry_us;
    if (s_relay.retry_us > CHECK_INTERVAL_US) {
        s_relay.retry_us = CHECK_INTERVAL_US;
    }
    s_relay.next_check_us = now_us() + s_relay.retry_us;
}

static void refresh(int64_t max_age_us)
{
    lock();
    const bool include_dev = s_relay.include_dev;
    const bool fresh = s_relay.fetched_us != 0 &&
                       now_us() - s_relay.fetched_us < max_age_us &&
                       (s_relay.dev_fetched || !include_dev);
    unlock();
    if (fresh) {
        return;
    }
    const char *error = fetch_index(STABLE);
    if (error == NULL && include_dev) {
        error = fetch_index(DEV);
    }
    if (error != NULL) {
        ESP_LOGW(TAG, "release index not refreshed: %s", error);
        lock();
        retry_locked();
        unlock();
        return;
    }
    lock();
    s_relay.fetched_us = now_us();
    s_relay.dev_fetched = include_dev;
    unlock();
}

/* --- Updates ------------------------------------------------------------- */

static const osvc_release_t *newest_locked(void)
{
    return osvc_release_newest(&s_relay.releases[STABLE],
                               &s_relay.releases[DEV], s_relay.include_dev);
}

static bool running_locked(void)
{
    return s_relay.progress.phase == OSVC_UPDATE_PHASE_DOWNLOADING ||
           s_relay.progress.phase == OSVC_UPDATE_PHASE_SENDING ||
           s_relay.progress.phase == OSVC_UPDATE_PHASE_INSTALLING;
}

static void finish_locked(osvc_update_phase_t phase, const char *error)
{
    s_relay.progress.phase = phase;
    snprintf(s_relay.progress.error, sizeof(s_relay.progress.error), "%s", error);
    changed_locked();
}

static void worker_main(void *arg);

static bool wake_worker_locked(void)
{
    if (s_relay.worker == NULL &&
        xTaskCreateWithCaps(&worker_main, "osvc_update", WORKER_STACK_BYTES,
                            NULL, WORKER_PRIORITY, &s_relay.worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_relay.worker = NULL;
        ESP_LOGW(TAG, "no memory for the update worker");
        return false;
    }
    return true;
}

static const char *start_locked(bool automatic)
{
    const osvc_release_t *newest = newest_locked();
    const osvc_release_package_t *package =
        osvc_release_target(newest, &s_relay.sender);
    if (package == NULL) {
        return UNAVAILABLE;
    }
    if (running_locked()) {
        return "an OS Services update is already in progress";
    }
    if (jetkvm_ota_is_update_pending()) {
        return FIRMWARE_UPDATING;
    }
    if (!wake_worker_locked()) {
        return "no memory for the OS Services update";
    }
    s_relay.package = *package;
    s_relay.update_session = s_relay.sender_session;
    s_relay.package_signed = newest->is_signed;
    s_relay.progress = (osvc_update_progress_t){
        .phase = OSVC_UPDATE_PHASE_DOWNLOADING,
        .size = package->size,
    };
    memcpy(s_relay.progress.version, newest->version,
           sizeof(s_relay.progress.version));
    if (automatic) {
        memcpy(s_relay.auto_tried, newest->version, sizeof(s_relay.auto_tried));
        s_relay.auto_tried_us = now_us();
    }
    s_relay.update_wanted = true;
    changed_locked();
    ESP_LOGI(TAG, "%s OS Services %s (%s)", automatic ? "auto-updating to" :
                                                         "updating to",
             newest->version, package->path);
    return NULL;
}

/* After a refresh: install by itself while auto-update is on and no browser
 * session is open; a version that did not install waits an hour. */
static void consider_auto_update(void)
{
    const bool browser = jetkvm_ota_session_active();
    lock();
    const osvc_release_t *newest = newest_locked();
    if (!browser && s_relay.automatic && newest != NULL &&
        (strcmp(s_relay.auto_tried, newest->version) != 0 ||
         now_us() - s_relay.auto_tried_us >= AUTO_RETRY_US)) {
        (void)start_locked(true);
    }
    unlock();
}

static bool begun(void)
{
    return s_relay.update_id != 0U || s_relay.ended;
}

/* Only the OS Services task ends these waits, a firmware install's stop
 * included, so no buffer is freed while it still sends from one. */
static bool buffer_free(void)
{
    return s_relay.chunk_count < 2U || s_relay.ended;
}

static bool settled(void)
{
    return s_relay.ended;
}

/* The sender has every byte and installs: a stop no longer fails it. */
static bool sender_installing(void)
{
    lock();
    const bool installing =
        s_relay.ended && s_relay.status == OSVC_UPDATE_STATUS_INSTALLING;
    unlock();
    return installing;
}

static void wait_until(bool (*done)(void))
{
    for (;;) {
        lock();
        const bool finished = done();
        unlock();
        if (finished) {
            return;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

/* The relay's io.send: queue the buffer for the OS Services task, then wait
 * until the one before it is out, so the other buffer can be refilled. The
 * sender may end the update before this returns, once it has every byte. */
static bool relay_send(void *ctx, const uint8_t *data, size_t length)
{
    (void)ctx;
    lock();
    const bool queued = !s_relay.ended;
    if (queued) {
        s_relay.chunks[s_relay.chunk_count] = data;
        s_relay.chunk_lengths[s_relay.chunk_count++] = length;
    }
    unlock();
    xTaskNotifyGive(s_relay.service_task);
    wait_until(buffer_free);
    return queued;
}

/* Hands a begin or a cancel (with its reason) to the OS Services task and
 * waits for done. */
static void request(bool *flag, const char *reason, bool (*done)(void))
{
    lock();
    *flag = true;
    snprintf(s_relay.cancel_reason, sizeof(s_relay.cancel_reason), "%s",
             reason);
    unlock();
    xTaskNotifyGive(s_relay.service_task);
    wait_until(done);
}

/* update.begin, the package through the checks, and the sender's verdict. */
static const char *transfer(http_t *http, osvc_package_check_t *check,
                            uint8_t *buffers)
{
    request(&s_relay.begin, "", begun);
    lock();
    const char *error = s_relay.ended ? s_relay.message : NULL;
    if (error == NULL) {
        s_relay.progress.phase = OSVC_UPDATE_PHASE_SENDING;
        changed_locked();
    }
    unlock();
    if (error != NULL) {
        osvc_package_check_abort(check);
        return error;
    }
    uint8_t *const halves[2] = {buffers, buffers + BUFFER_BYTES};
    const osvc_relay_io_t io = {http, http_read, relay_send};
    error = osvc_relay_package(&io, check, halves, signature_time());
    if (s_relay.stop && !sender_installing()) {
        error = STOPPED;
    }
    if (error != NULL) {
        request(&s_relay.cancel, error, settled);
        return error;
    }
    wait_until(settled);
    if (sender_installing()) {
        return NULL;
    }
    lock();
    error = s_relay.stop ? STOPPED : s_relay.message;
    unlock();
    return error;
}

/* Downloads, checks and relays the chosen package. */
static const char *relay_package(const osvc_release_package_t *package,
                                 bool is_signed)
{
    char url[URL_CAPACITY];
    response_t signature = {0};
    const char *error = NULL;
    if (is_signed) {
        file_url(url, package->path, ".sig");
        error = fetch(url, "", SIGNATURE_MAX, &signature);
        if (error == NULL && signature.status != 200) {
            error = "the package has no signature";
        }
        error = error != NULL ? error : release_keys();
    }
    http_t http = {0};
    int64_t length = 0;
    file_url(url, package->path, "");
    error = error != NULL ? error :
                            http_open(&http, url, "", true, &length);
    if (error == NULL && (esp_http_client_get_status_code(http.client) != 200 ||
                          (uint64_t)length != package->size)) {
        error = "the package is not on the release server";
    }
    uint8_t *buffers = error == NULL ? psram_alloc(2U * BUFFER_BYTES) : NULL;
    if (error == NULL && buffers == NULL) {
        error = "no memory to relay the package";
    }
    osvc_package_check_t check;
    error = error != NULL ? error :
            osvc_package_check_begin(&check, &osvc_psa_hash_ops, package->size,
                                     package->sha256, signature.body,
                                     signature.length, s_relay.keys,
                                     s_relay.key_count);
    if (error == NULL) {
        error = transfer(&http, &check, buffers);
    }
    heap_caps_free(buffers);
    http_close(&http);
    heap_caps_free(signature.body);
    return error;
}

static void stop_for_install(void *ctx)
{
    (void)ctx;
    lock();
    __atomic_store_n(&s_relay.stop, true, __ATOMIC_RELEASE);
    unlock();
    xTaskNotifyGive(s_relay.service_task);
}

static void run_update(void)
{
    lock();
    /* start_locked() refuses while this runs, so the package stays. */
    const osvc_release_package_t *package = &s_relay.package;
    const bool is_signed = s_relay.package_signed;
    s_relay.begin = s_relay.cancel = s_relay.ended = s_relay.stop = false;
    s_relay.update_id = 0U;
    s_relay.chunk_count = s_relay.chunk_sent = 0U;
    /* No verdict from the sender yet. */
    s_relay.status = OSVC_UPDATE_STATUS_CANCELLED;
    unlock();
    const char *error = FIRMWARE_UPDATING;
    if (jetkvm_ota_side_download_begin(stop_for_install, NULL)) {
        error = relay_package(package, is_signed);
        jetkvm_ota_side_download_end();
    }
    lock();
    /* What the session still says about this update is no longer heard. */
    s_relay.ended = true;
    s_relay.chunk_count = 0U;
    if (error != NULL) {
        ESP_LOGW(TAG, "update failed: %s", error);
        finish_locked(OSVC_UPDATE_PHASE_FAILED, error);
    }
    unlock();
}

static void worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        lock();
        const bool refresh_wanted = s_relay.refresh_wanted;
        const bool update_wanted = s_relay.update_wanted;
        const int64_t max_age_us = s_relay.refresh_max_age_us;
        s_relay.refresh_wanted = s_relay.update_wanted = false;
        /* A stopped relay's last request goes with the stop. */
        s_relay.begin = s_relay.cancel = s_relay.stop = false;
        if (!refresh_wanted && !update_wanted) {
            s_relay.worker = NULL;
            unlock();
            break;
        }
        unlock();
        /* The index downloads give way to a firmware install too; the next
         * check retries one it refused. */
        if (refresh_wanted &&
            jetkvm_ota_side_download_begin(stop_for_install, NULL)) {
            refresh(max_age_us);
            jetkvm_ota_side_download_end();
            consider_auto_update();
        }
        if (update_wanted) {
            run_update();
        }
    }
    ESP_LOGI(TAG, "worker done; %u bytes of stack unused",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDeleteWithCaps(NULL);
}

/* --- The OS Services task's turn ---------------------------------------- */

static void request_refresh_locked(int64_t max_age_us)
{
    s_relay.refresh_max_age_us =
        s_relay.refresh_wanted && s_relay.refresh_max_age_us < max_age_us ?
            s_relay.refresh_max_age_us :
            max_age_us;
    s_relay.refresh_wanted = true;
    s_relay.next_check_us = now_us() + CHECK_INTERVAL_US;
    (void)wake_worker_locked();
}

/* A sender reported in a new session: an update that was installing ends
 * with its version, one that is done no longer describes another version,
 * and the indexes are refreshed. */
static void sender_connected_locked(void)
{
    osvc_update_progress_t *progress = &s_relay.progress;
    const bool same = strcmp(s_relay.sender.version, progress->version) == 0;
    if (progress->phase == OSVC_UPDATE_PHASE_DONE && !same) {
        *progress = (osvc_update_progress_t){0};
        changed_locked();
    } else if ((progress->phase == OSVC_UPDATE_PHASE_INSTALLING &&
         s_relay.sender_session != s_relay.update_session) ||
        (progress->phase == OSVC_UPDATE_PHASE_FAILED && same)) {
        finish_locked(same ? OSVC_UPDATE_PHASE_DONE : OSVC_UPDATE_PHASE_FAILED,
                      same ? "" : "OS Services restarted with another version");
    }
    request_refresh_locked(CONNECT_MAX_AGE_US);
}

static void follow_sender_locked(const osvc_session_t *session)
{
    osvc_update_sender_t sender = {0};
    if (osvc_session_open(session) && session->sender_info_valid) {
        memcpy(sender.version, session->sender_version, sizeof(sender.version));
        if (session->update_install) {
            memcpy(sender.package, session->update_package,
                   sizeof(sender.package));
        }
    }
    if (memcmp(&sender, &s_relay.sender, sizeof(sender)) != 0) {
        s_relay.sender = sender;
        changed_locked();
    }
    if (sender.version[0] != '\0' &&
        s_relay.sender_session != session->counters.sessions_opened) {
        s_relay.sender_session = session->counters.sessions_opened;
        sender_connected_locked();
    }
    if (s_relay.progress.phase == OSVC_UPDATE_PHASE_INSTALLING &&
        now_us() > s_relay.installing_deadline_us) {
        finish_locked(OSVC_UPDATE_PHASE_FAILED,
                      "OS Services did not start again in 3 minutes");
    }
    if (sender.version[0] != '\0' && now_us() >= s_relay.next_check_us) {
        request_refresh_locked(INDEX_MAX_AGE_US);
    }
}

static void follow_settings_locked(const jetkvm_ota_config_t *config)
{
    if (config->auto_update_enabled != s_relay.automatic ||
        config->include_prerelease != s_relay.include_dev) {
        s_relay.automatic = config->auto_update_enabled;
        s_relay.include_dev = config->include_prerelease;
        changed_locked();
        if (s_relay.sender.version[0] != '\0') {
            request_refresh_locked(INDEX_MAX_AGE_US);
        }
    }
}

/* The worker's begin and cancel, and a firmware install's stop, on the task
 * that writes to the sender. */
static void serve_requests(osvc_session_t *session)
{
    lock();
    const bool stop = s_relay.stop && !s_relay.ended;
    const bool begin = s_relay.begin && !s_relay.stop;
    const bool same_sender =
        session->counters.sessions_opened == s_relay.update_session;
    const bool cancel = s_relay.cancel || stop;
    const uint32_t current = s_relay.update_id;
    const uint64_t size = s_relay.package.size;
    uint8_t sha256[32];
    char version[OSVC_UPDATE_VERSION_MAX + 1U];
    char reason[OSVC_UPDATE_MESSAGE_CAPACITY];
    memcpy(sha256, s_relay.package.sha256, sizeof(sha256));
    memcpy(version, s_relay.progress.version, sizeof(version));
    snprintf(reason, sizeof(reason), "%s", stop ? STOPPED : s_relay.cancel_reason);
    s_relay.begin = s_relay.cancel = false;
    unlock();
    uint32_t update_id = 0U;
    const char *error =
        !begin ? NULL :
        !same_sender ? "the OS Services sender changed" :
                       osvc_session_update_begin(session, size, sha256, version,
                                                 &update_id);
    if (cancel) {
        osvc_session_update_cancel(session, current, reason);
    }
    lock();
    if (begin) {
        s_relay.update_id = update_id;
        s_relay.ended = error != NULL;
        snprintf(s_relay.message, sizeof(s_relay.message), "%s",
                 error != NULL ? error : "");
        notify_worker_locked();
    }
    if (stop) {
        /* No chunk is in flight between serve_chunks() turns. */
        s_relay.ended = true;
        s_relay.chunk_count = 0U;
        snprintf(s_relay.message, sizeof(s_relay.message), "%s", STOPPED);
        notify_worker_locked();
    }
    unlock();
}

/* Relays the queued buffers as far as the sender's credit allows. */
static void serve_chunks(osvc_session_t *session)
{
    for (;;) {
        lock();
        const uint32_t update_id = s_relay.update_id;
        const size_t count = s_relay.chunk_count;
        const uint8_t *data =
            count > 0U ? s_relay.chunks[0] + s_relay.chunk_sent : NULL;
        const size_t left =
            count > 0U ? s_relay.chunk_lengths[0] - s_relay.chunk_sent : 0U;
        unlock();
        const size_t sent =
            data != NULL ?
                osvc_session_update_send(session, update_id, data, left) :
                0U;
        lock();
        s_relay.chunk_sent += sent;
        const bool popped = count > 0U && sent == left;
        if (popped) {
            s_relay.chunks[0] = s_relay.chunks[1];
            s_relay.chunk_lengths[0] = s_relay.chunk_lengths[1];
            s_relay.chunk_count--;
            s_relay.chunk_sent = 0U;
            notify_worker_locked();
        }
        unlock();
        if (!popped) {
            return;
        }
    }
}

/* The update's outcome and progress, from the session. */
static void follow_update_locked(const osvc_session_t *session)
{
    const osvc_update_t *update = &session->update;
    if (s_relay.update_id == 0U || s_relay.ended ||
        update->update_id != s_relay.update_id) {
        return;
    }
    if (update->ended) {
        s_relay.ended = true;
        s_relay.status = update->status;
        memcpy(s_relay.message, update->message, sizeof(s_relay.message));
        s_relay.chunk_count = 0U;
        notify_worker_locked();
        /* The sender restarts; its next session reports the version, maybe
         * before the worker has closed its download. */
        if (update->status == OSVC_UPDATE_STATUS_INSTALLING) {
            s_relay.installing_deadline_us = now_us() + RESTART_TIMEOUT_US;
            finish_locked(OSVC_UPDATE_PHASE_INSTALLING, "");
        }
    }
    /* Progress at most once a second. */
    if (update->sent != s_relay.progress.sent &&
        (update->sent == update->size ||
         now_us() - s_relay.progress_pushed_us >= SECOND_US)) {
        s_relay.progress.sent = update->sent;
        s_relay.progress_pushed_us = now_us();
        changed_locked();
    }
}

bool osvc_relay_service(osvc_session_t *session)
{
    if (s_relay.lock == NULL) {
        return false;
    }
    jetkvm_ota_config_t config = {0};
    (void)jetkvm_ota_get_config(&config);
    serve_requests(session);
    serve_chunks(session);
    lock();
    follow_update_locked(session);
    follow_sender_locked(session);
    follow_settings_locked(&config);
    const bool push = s_relay.generation != s_relay.pushed_generation;
    s_relay.pushed_generation = s_relay.generation;
    unlock();
    return push;
}

uint32_t osvc_relay_generation(void)
{
    return __atomic_load_n(&s_relay.generation, __ATOMIC_ACQUIRE);
}

const char *osvc_relay_start(void)
{
    if (s_relay.lock == NULL) {
        return UNAVAILABLE;
    }
    lock();
    const char *error = start_locked(false);
    unlock();
    return error;
}

bool osvc_relay_add_state(cJSON *object)
{
    if (s_relay.lock == NULL) {
        return true;
    }
    lock();
    const bool ok = osvc_update_render(object, newest_locked(), &s_relay.sender,
                                       &s_relay.progress, s_relay.automatic);
    unlock();
    return ok;
}

esp_err_t osvc_relay_init(TaskHandle_t service_task)
{
    s_relay.service_task = service_task;
    s_relay.lock = xSemaphoreCreateMutex();
    return s_relay.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}
