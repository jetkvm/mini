/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The update relay's worker and the OS Services task as two threads, with
 * the package served by a fake HTTP client and the sender by a fake
 * session that reads every byte it is handed (AddressSanitizer reports a
 * freed chunk). A whole relay reaches the sender byte for byte; a firmware
 * install that stops it frees no chunk the OS Services task still reads and
 * begins no update on the sender, and no download opens after the stop; a
 * stop after the sender has every byte leaves the update installing; no
 * index download overlaps an install; a stop during an index body ends the
 * read; an update the worker cannot start for want of memory, or that a
 * firmware install would stop, says so. An update that is done ends when a
 * sender with another version reports in. A first index check that finds no
 * network is tried again soon, backing off, and once an index is held the
 * checks keep their interval.
 */

#include "os_services_relay.c"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

#define PACKAGE_BYTES (3U * BUFFER_BYTES + 1000U)

typedef enum {
    STOP_NEVER, STOP_AT_HEADERS, STOP_IN_READ, STOP_IN_SEND, STOP_AFTER_INSTALL,
    RECONNECT, NEW_SENDER
} stop_at_t;

static uint8_t s_package[PACKAGE_BYTES];
static size_t s_read;
static int s_opens;
static int s_status = 200;
static bool s_network_down;
static bool s_installing;
static stop_at_t s_stop_at;
static void (*s_stop)(void *ctx);
static void *s_stop_ctx;
static osvc_session_t s_session;
static int s_begins;
static atomic_bool s_worker_done;
/* Another sender connected while the worker opened the download; the OS
 * Services task's session sees it on its next pass. */
static atomic_bool s_new_sender;

/* A firmware install while the OS Services task holds a chunk: stop the
 * relay and wait for it, at most a second (the OTA task waits up to 35 s). */
static void install_firmware(void)
{
    s_stop(s_stop_ctx);
    for (int ms = 0; ms < 1000 && !atomic_load(&s_worker_done); ms += 10) {
        (void)ulTaskNotifyTake(pdFALSE, 10U);
    }
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    static int client;
    return (esp_http_client_handle_t)&client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value)
{
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    s_opens++;
    s_read = 0U;
    atomic_store(&s_new_sender, s_stop_at == NEW_SENDER);
    return s_network_down ? ESP_FAIL : ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    if (s_stop_at == STOP_AT_HEADERS) {
        s_stop(s_stop_ctx);
    }
    return PACKAGE_BYTES;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return s_status;
}
/* As ESP-IDF's, it returns only once it has all it was asked for. */
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    if (s_stop_at == STOP_IN_READ && s_read == 0U) {
        s_stop(s_stop_ctx);
    }
    const size_t n = PACKAGE_BYTES - s_read < (size_t)len ? PACKAGE_BYTES - s_read :
                                                            (size_t)len;
    memcpy(buffer, s_package + s_read, n);
    s_read += n;
    return (int)n;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    return ESP_OK;
}

bool jetkvm_ota_side_download_begin(void (*stop)(void *ctx), void *ctx)
{
    s_stop = stop;
    s_stop_ctx = ctx;
    return !s_installing;
}
void jetkvm_ota_side_download_end(void)
{
    s_stop = NULL;
}
esp_err_t jetkvm_ota_get_config(jetkvm_ota_config_t *config)
{
    memset(config, 0, sizeof(*config));
    return ESP_OK;
}
bool jetkvm_ota_session_active(void)
{
    return false;
}
bool jetkvm_ota_is_update_pending(void)
{
    return s_installing;
}
/* No test here reaches a signature check. */
const char *jetkvm_ota_release_signing_keys(
    bool (*stopped)(void), osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX],
    size_t *count, char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    assert(!"a signature check");
    return NULL;
}

/* The sender, as the session tracks it. */
bool osvc_session_open(const osvc_session_t *session)
{
    return true;
}
const char *osvc_session_update_begin(osvc_session_t *session, uint64_t size,
                                      const uint8_t sha256[32],
                                      const char *version, uint32_t *update_id)
{
    s_begins++;
    session->update = (osvc_update_t){.update_id = 7U, .size = size};
    *update_id = 7U;
    return NULL;
}
size_t osvc_session_update_send(osvc_session_t *session, uint32_t update_id,
                                const uint8_t *data, size_t length)
{
    osvc_update_t *update = &session->update;
    assert(update_id == update->update_id && !update->ended);
    assert(update->sent + length <= update->size);
    if (s_stop_at == STOP_IN_SEND && update->sent == 0U) {
        install_firmware();
    }
    assert(memcmp(data, s_package + update->sent, length) == 0);
    update->sent += length;
    if (update->sent == update->size) {
        update->ended = true;
        update->status = OSVC_UPDATE_STATUS_INSTALLING;
        if (s_stop_at == STOP_AFTER_INSTALL) {
            s_stop(s_stop_ctx);
        }
    }
    /* The sender restarts with the new version at once, before the worker
     * has closed its download. */
    if (s_stop_at == RECONNECT && update->ended) {
        session->counters.sessions_opened++;
        session->sender_info_valid = true;
        snprintf(session->sender_version, sizeof(session->sender_version),
                 "%s", s_relay.progress.version);
    }
    return length;
}
void osvc_session_update_cancel(osvc_session_t *session, uint32_t update_id,
                                const char *reason)
{
    osvc_update_t *update = &session->update;
    if (update_id == update->update_id && !update->ended) {
        update->ended = true;
        update->status = OSVC_UPDATE_STATUS_CANCELLED;
        snprintf(update->message, sizeof(update->message), "%s", reason);
    }
}

/* The worker task, as xTaskCreateWithCaps() would start it. */
static void *worker(void *arg)
{
    lock();
    s_relay.worker = fake_task_self();
    unlock();
    worker_main(NULL);
    atomic_store(&s_worker_done, true);
    xTaskNotifyGive(s_relay.service_task);
    return NULL;
}

/* Runs the worker's jobs with this thread as the OS Services task. */
static void run_worker(void)
{
    atomic_store(&s_worker_done, false);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, worker, NULL) == 0);
    while (!atomic_load(&s_worker_done)) {
        (void)ulTaskNotifyTake(pdTRUE, 10U);
        s_session.counters.sessions_opened += atomic_exchange(&s_new_sender, false);
        (void)osvc_relay_service(&s_session);
    }
    (void)osvc_relay_service(&s_session);
    pthread_join(thread, NULL);
}

static void relay(stop_at_t stop_at)
{
    s_stop_at = stop_at;
    s_begins = 0;
    s_session = (osvc_session_t){0};
    lock();
    s_relay.package = (osvc_release_package_t){.path = "x.AppImage",
                                               .size = PACKAGE_BYTES};
    EVP_Digest(s_package, PACKAGE_BYTES, s_relay.package.sha256, NULL,
               EVP_sha256(), NULL);
    s_relay.package_signed = false;
    /* Chosen for the connected sender, as start_locked() does. */
    s_relay.update_session = s_session.counters.sessions_opened;
    s_relay.update_wanted = true;
    unlock();
    run_worker();
}

/* Moves the clock on, gives the OS Services task a turn and runs the index
 * check it asks for, if any: the number of downloads opened. */
static int check_after(int seconds)
{
    __atomic_add_fetch(&fake_time_skipped_us, seconds * SECOND_US,
                       __ATOMIC_RELEASE);
    s_opens = 0;
    (void)osvc_relay_service(&s_session);
    run_worker();
    return s_opens;
}

/* A sender reports in at boot, before the network is up. */
static void check_index_at_boot(void)
{
    s_session = (osvc_session_t){.sender_info_valid = true};
    snprintf(s_session.sender_version, sizeof(s_session.sender_version), "1.0.0");
    s_session.counters.sessions_opened = 100U;
    s_network_down = true;
    assert(check_after(0) == 1);
    /* Tried again in 15 s, then 30 s. */
    assert(check_after(14) == 0);
    assert(check_after(2) == 1);
    assert(check_after(16) == 0);
    /* The network is up: the next try gets the index (unchanged, so it
     * is not read). */
    s_network_down = false;
    s_status = 304;
    assert(check_after(15) == 1);
    /* An index is held: no check before the interval, and no download
     * at it while the index is less than an hour old. */
    assert(check_after(250) == 0);
    assert(check_after(60) == 0);
    lock();
    assert(s_relay.fetched_us != 0 && !s_relay.refresh_wanted);
    s_relay.fetched_us = 0;
    unlock();
    s_status = 200;
    s_session = (osvc_session_t){0};
    (void)osvc_relay_service(&s_session);
}

/* The guestUpdate state the browser reads; the caller frees it. */
static char *rendered_state(void)
{
    cJSON *object = cJSON_CreateObject();
    assert(object != NULL && osvc_relay_add_state(object));
    char *text = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    return text;
}

/* After an update is done, a new session with the same version keeps it
 * done; a new session with an older build outside the relay clears it, and
 * the release is offered to that build again. */
static void sender_replaced_after_done(void)
{
    lock();
    s_relay.releases[STABLE] = (osvc_release_t){
        .version = "2.0.0", .package_count = 1U,
        .packages = {{.name = "linux-x86_64", .size = PACKAGE_BYTES}}};
    unlock();
    s_session.update_install = true;
    snprintf(s_session.update_package, sizeof(s_session.update_package),
             "linux-x86_64");
    s_session.counters.sessions_opened++;
    (void)osvc_relay_service(&s_session);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_DONE);

    s_session.counters.sessions_opened++;
    snprintf(s_session.sender_version, sizeof(s_session.sender_version),
             "2.0.0-dev202610061427");
    (void)osvc_relay_service(&s_session);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_NONE);
    char *text = rendered_state();
    assert(strstr(text, "\"version\":\"2.0.0\",\"available\":true,"
                        "\"installable\":true,") != NULL);
    assert(strstr(text, "\"phase\"") == NULL);
    cJSON_free(text);

    lock();
    s_relay.releases[STABLE] = (osvc_release_t){0};
    s_relay.refresh_wanted = false;
    unlock();
}

int main(void)
{
    for (size_t i = 0U; i < PACKAGE_BYTES; ++i) {
        s_package[i] = (uint8_t)(i * 7U + i / 251U);
    }
    assert(osvc_relay_init(fake_task_self()) == ESP_OK);

    check_index_at_boot();

    relay(STOP_NEVER);
    assert(s_begins == 1);
    assert(s_session.update.sent == PACKAGE_BYTES);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_INSTALLING);

    /* A stop after the sender has every byte: still installing. */
    relay(STOP_AFTER_INSTALL);
    assert(s_session.update.status == OSVC_UPDATE_STATUS_INSTALLING);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_INSTALLING);

    /* The new version reports in while the worker still closes: done. */
    lock();
    snprintf(s_relay.progress.version, sizeof(s_relay.progress.version), "2.0.0");
    unlock();
    relay(RECONNECT);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_DONE);
    sender_replaced_after_done();

    /* Another sender connected during HTTPS setup: no begin for a package
     * chosen for the one before. */
    relay(NEW_SENDER);
    assert(s_begins == 0);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_FAILED);
    assert(strcmp(s_relay.progress.error, "the OS Services sender changed") == 0);

    relay(STOP_IN_SEND);
    assert(s_begins == 1);
    assert(s_session.update.ended && s_session.update.sent < PACKAGE_BYTES);
    assert(s_session.update.status == OSVC_UPDATE_STATUS_CANCELLED);
    assert(strcmp(s_session.update.message, STOPPED) == 0);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_FAILED);
    assert(strcmp(s_relay.progress.error, STOPPED) == 0);

    relay(STOP_AT_HEADERS);
    assert(s_begins == 0);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_FAILED);
    assert(strcmp(s_relay.progress.error, STOPPED) == 0);

    /* A firmware install is running: the index check waits for it. */
    s_installing = true;
    s_stop_at = STOP_NEVER;
    s_opens = 0;
    lock();
    s_relay.refresh_wanted = true;
    unlock();
    run_worker();
    assert(s_opens == 0);
    s_installing = false;

    /* A stop at the stable index's headers, which say unchanged so nothing
     * is read: the dev index does not open after it. */
    s_status = 304;
    s_stop_at = STOP_AT_HEADERS;
    s_opens = 0;
    lock();
    s_relay.include_dev = true;
    s_relay.refresh_wanted = true;
    unlock();
    run_worker();
    assert(s_opens == 1);
    s_status = 200;
    s_relay.include_dev = false;

    /* A stop while a slow index body arrives: the read ends without the
     * rest, so the install does not wait for the whole download. */
    s_stop_at = STOP_IN_READ;
    s_opens = 0;
    lock();
    s_relay.refresh_wanted = true;
    unlock();
    run_worker();
    assert(s_opens == 1);
    assert(s_read < PACKAGE_BYTES);

    /* No memory for the worker (xTaskCreateWithCaps fails here). */
    lock();
    s_relay.releases[STABLE] = (osvc_release_t){
        .version = "9.0.0", .package_count = 1U,
        .packages = {{.name = "linux-x86_64", .size = PACKAGE_BYTES}}};
    s_relay.sender = (osvc_update_sender_t){.version = "1.0.0",
                                            .package = "linux-x86_64"};
    unlock();
    const char *error = osvc_relay_start();
    assert(error != NULL && strcmp(error, "no memory for the OS Services update") == 0);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_FAILED);

    /* A firmware install is on its way: refused before the worker starts. */
    s_installing = true;
    error = osvc_relay_start();
    assert(error != NULL && strcmp(error, "a firmware update is in progress") == 0);
    assert(s_relay.progress.phase == OSVC_UPDATE_PHASE_FAILED);
    s_installing = false;

    puts("os_services relay tests passed");
    return 0;
}
