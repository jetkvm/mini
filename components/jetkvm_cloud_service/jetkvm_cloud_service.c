/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_cloud_service.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_cloud_service_policy.h"
#include "jetkvm_device_state.h"
#include "jetkvm_board_config.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_time_sync.h"

#define CLOUD_SERVICE_TASK_STACK       8192U
#define CLOUD_SERVICE_TASK_PRIORITY    5U
#define CLOUD_SERVICE_WORKER_STOPPED_BIT BIT0
#define CLOUD_SERVICE_STOP_COMPLETE_BIT  BIT1
#define CLOUD_SERVICE_START_RESOLVED_BIT BIT2
#define CLOUD_SERVICE_TIME_WAIT_MS     1000U
#define CLOUD_SERVICE_RETRY_MS         5000U

typedef enum {
    CLOUD_SERVICE_STOPPED = 0,
    CLOUD_SERVICE_STARTING,
    CLOUD_SERVICE_RUNNING,
    CLOUD_SERVICE_STOPPING,
} cloud_service_lifecycle_t;

typedef struct {
    /* These RTOS objects deliberately live for the process lifetime. A state
     * observer may already have captured its callback just before unsubscribe;
     * retaining the queue and lock makes that late, nonblocking callback safe. */
    SemaphoreHandle_t lock;
    QueueHandle_t changes;
    EventGroupHandle_t events;
    bool resources_ready;

    TaskHandle_t task;
    jetkvm_oidc_validator_t *oidc;
    jetkvm_cloud_client_handle_t client;
    uint32_t active_change_epoch;
    uint32_t active_generation;
    char active_identity[JETKVM_CLOUD_IDENTITY_CAPACITY];
    jetkvm_cloud_service_stats_t stats;

    atomic_uint lifecycle;
    atomic_uint change_epoch;
    atomic_bool stop_requested;
} cloud_service_t;

static const char *TAG = "jetkvm_cloud_svc";
static cloud_service_t s_service = {
    .lifecycle = ATOMIC_VAR_INIT(CLOUD_SERVICE_STOPPED),
    .change_epoch = ATOMIC_VAR_INIT(0U),
    .stop_requested = ATOMIC_VAR_INIT(false),
};

static void secure_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (bytes && size--) {
        *bytes++ = 0U;
    }
}

static cloud_service_lifecycle_t service_lifecycle(void)
{
    return (cloud_service_lifecycle_t)atomic_load_explicit(
        &s_service.lifecycle, memory_order_acquire);
}

static bool service_running(void)
{
    return service_lifecycle() == CLOUD_SERVICE_RUNNING;
}

static bool service_stopping(void)
{
    return atomic_load_explicit(&s_service.stop_requested,
                                memory_order_acquire) ||
           service_lifecycle() != CLOUD_SERVICE_RUNNING;
}

static uint32_t current_change_epoch(void)
{
    return atomic_load_explicit(&s_service.change_epoch,
                                memory_order_acquire);
}

static bool snapshot_current(uint32_t change_epoch)
{
    return jetkvm_cloud_service_policy_snapshot_current(
        change_epoch, current_change_epoch(), service_running(),
        atomic_load_explicit(&s_service.stop_requested,
                             memory_order_acquire));
}

static bool device_generation_current(uint32_t generation)
{
    jetkvm_device_cloud_public_state_t state = { 0 };
    return generation != 0U &&
           jetkvm_device_state_cloud_public_snapshot(&state) == ESP_OK &&
           state.registered && state.generation == generation;
}

static void drain_change_queue(void)
{
    uint8_t wake = 0U;
    while (xQueueReceive(s_service.changes, &wake, 0) == pdTRUE) {
    }
}

static void config_changed(uint32_t changes, uint32_t generation, void *ctx)
{
    (void)changes;
    (void)generation;
    cloud_service_t *service = ctx;
    cloud_service_lifecycle_t lifecycle = service_lifecycle();
    if (service != &s_service || !service->resources_ready ||
        lifecycle == CLOUD_SERVICE_STOPPED) {
        return;
    }

    /* A monotonic local epoch is used instead of the supplied generation.
     * Device-state callbacks run after its mutex is released and two callers
     * may therefore deliver generations out of order. Incrementing cannot
     * regress freshness, while the queue remains only a wake-up mechanism. */
    (void)atomic_fetch_add_explicit(&service->change_epoch, 1U,
                                    memory_order_acq_rel);
    uint8_t wake = 1U;
    (void)xQueueOverwrite(service->changes, &wake);
}

static bool oidc_status_is_retryable(jetkvm_oidc_status_t status)
{
    switch (status) {
    case JETKVM_OIDC_INVALID_JWKS:
    case JETKVM_OIDC_JWKS_UNAVAILABLE:
    case JETKVM_OIDC_CLOCK_NOT_READY:
    case JETKVM_OIDC_BUSY:
    case JETKVM_OIDC_NO_MEMORY:
    case JETKVM_OIDC_CRYPTO_ERROR:
        /* These describe local capacity/readiness or an unavailable
         * authoritative JWKS document, not evidence that the offered token
         * is invalid. Preserve the offer through the bounded retry path. */
        return true;
    default:
        return false;
    }
}

static jetkvm_cloud_identity_validation_t validate_session_identity(
    const char *oidc_google, size_t token_length, void *ctx)
{
    cloud_service_t *service = ctx;
    if (service != &s_service ||
        !jetkvm_cloud_service_policy_token_shape_valid(
            oidc_google, token_length, JETKVM_OIDC_MAX_TOKEN_BYTES)) {
        return JETKVM_CLOUD_IDENTITY_INVALID;
    }

    jetkvm_oidc_validator_t *oidc = NULL;
    bool preliminarily_current = false;
    xSemaphoreTake(service->lock, portMAX_DELAY);
    oidc = service->oidc;
    preliminarily_current =
        oidc && jetkvm_cloud_service_policy_offer_current(
                    service->active_change_epoch, current_change_epoch(),
                    service->active_generation, service_running(),
                    atomic_load_explicit(&service->stop_requested,
                                         memory_order_acquire),
                    service->client != NULL);
    xSemaphoreGive(service->lock);
    if (!preliminarily_current) {
        return JETKVM_CLOUD_IDENTITY_INVALID;
    }

    /* Session validation may fetch the key set over HTTPS and always checks
     * an RSA signature. The fetch is one TLS connection, so it takes a
     * connection's reservation; the signature check alone takes no internal
     * RAM. */
    jetkvm_memory_policy_reservation_t oidc_memory = {0};
    if (!jetkvm_memory_policy_reserve_tls(
            "cloud-session-oidc", &oidc_memory)) {
        ESP_LOGW(TAG, "cloud offer OIDC deferred: TLS memory unavailable");
        return JETKVM_CLOUD_IDENTITY_RETRYABLE;
    }

    char identity[JETKVM_CLOUD_IDENTITY_CAPACITY] = { 0 };
    jetkvm_oidc_status_t status = jetkvm_oidc_validate_session(
        oidc, oidc_google, identity, sizeof(identity));
    jetkvm_memory_policy_release(&oidc_memory);
    if (status != JETKVM_OIDC_OK) {
        ESP_LOGW(TAG, "cloud offer OIDC rejected: %s",
                 jetkvm_oidc_status_name(status));
        secure_zero(identity, sizeof(identity));
        return oidc_status_is_retryable(status)
                   ? JETKVM_CLOUD_IDENTITY_RETRYABLE
                   : JETKVM_CLOUD_IDENTITY_INVALID;
    }

    uint32_t active_generation = 0U;
    xSemaphoreTake(service->lock, portMAX_DELAY);
    active_generation = service->active_generation;
    xSemaphoreGive(service->lock);
    bool generation_current =
        device_generation_current(active_generation);

    /* This second gate is the linearization point: a config notification that
     * arrived while HTTPS/JWKS validation was in progress invalidates the old
     * registration before the offer can replace the active WebRTC peer. The
     * direct state-generation read also closes the short interval between a
     * persisted state publication and delivery of its observer callback. */
    xSemaphoreTake(service->lock, portMAX_DELAY);
    bool accepted = generation_current && service->oidc == oidc &&
        service->active_generation == active_generation &&
        jetkvm_cloud_service_policy_offer_current(
            service->active_change_epoch, current_change_epoch(),
            service->active_generation, service_running(),
            atomic_load_explicit(&service->stop_requested,
                                 memory_order_acquire),
            service->client != NULL) &&
        jetkvm_cloud_service_policy_identity_equal(
            identity, service->active_identity,
            sizeof(service->active_identity));
    xSemaphoreGive(service->lock);
    secure_zero(identity, sizeof(identity));
    return accepted ? JETKVM_CLOUD_IDENTITY_VALID :
                      JETKVM_CLOUD_IDENTITY_INVALID;
}

static void save_client_stats(jetkvm_cloud_client_handle_t client)
{
    jetkvm_cloud_client_stats_t stats = { 0 };
    if (!client || jetkvm_cloud_client_get_stats(client, &stats) != ESP_OK) {
        return;
    }
    /* Kept after the client is destroyed, so its WebSocket is gone. */
    stats.connected = false;
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.stats.client = stats;
    xSemaphoreGive(s_service.lock);
}

static esp_err_t destroy_client(jetkvm_cloud_client_handle_t client)
{
    if (!client) {
        return ESP_OK;
    }
    save_client_stats(client);
    esp_err_t err = jetkvm_cloud_client_destroy(client);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    /* cloud_client_stop() completes worker teardown and clears `started` even
     * when the underlying websocket destroy reports an error. A second destroy
     * therefore releases the still-owned client object without racing a task. */
    ESP_LOGW(TAG, "cloud client first destroy failed: %s; retrying cleanup",
             esp_err_to_name(err));
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.stats.destroy_failures++;
    xSemaphoreGive(s_service.lock);
    esp_err_t retry = jetkvm_cloud_client_destroy(client);
    if (retry != ESP_OK) {
        ESP_LOGE(TAG, "cloud client cleanup failed: %s",
                 esp_err_to_name(retry));
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        s_service.stats.destroy_failures++;
        xSemaphoreGive(s_service.lock);
    }
    return retry;
}

static jetkvm_cloud_client_handle_t detach_client(
    jetkvm_cloud_client_handle_t expected)
{
    jetkvm_cloud_client_handle_t client = NULL;
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (!expected || s_service.client == expected) {
        client = s_service.client;
        s_service.client = NULL;
        s_service.stats.client_running = false;
        s_service.active_change_epoch = 0U;
        s_service.active_generation = 0U;
        secure_zero(s_service.active_identity,
                    sizeof(s_service.active_identity));
    }
    xSemaphoreGive(s_service.lock);
    return client;
}

static void stop_active_client(void)
{
    jetkvm_cloud_client_handle_t client = detach_client(NULL);
    if (client) {
        (void)destroy_client(client);
    }
}

static esp_err_t start_client_for_snapshot(
    const jetkvm_cloud_config_t *config, uint32_t generation,
    uint32_t change_epoch, const char *device_id)
{
    if (!snapshot_current(change_epoch) ||
        !device_generation_current(generation)) {
        return ESP_ERR_INVALID_STATE;
    }

    const jetkvm_cloud_client_config_t client_config = {
        .url = config->cloud_url,
        .device_id = device_id,
        .app_version = esp_app_get_description()->version,
        .sku = JETKVM_DEVICE_SKU,
        .token = config->cloud_token,
        .validate_identity = validate_session_identity,
        .identity_ctx = &s_service,
    };
    jetkvm_cloud_client_handle_t client = NULL;
    esp_err_t err = jetkvm_cloud_client_init(&client_config, &client);
    if (err != ESP_OK) {
        return err;
    }

    bool generation_current = device_generation_current(generation);
    bool published = false;
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (!s_service.client && generation_current &&
        snapshot_current(change_epoch)) {
        s_service.client = client;
        s_service.active_change_epoch = change_epoch;
        s_service.active_generation = generation;
        memcpy(s_service.active_identity, config->google_identity,
               sizeof(s_service.active_identity));
        published = true;
    }
    xSemaphoreGive(s_service.lock);
    if (!published) {
        (void)destroy_client(client);
        return ESP_ERR_INVALID_STATE;
    }

    err = jetkvm_cloud_client_start(client);
    if (err != ESP_OK) {
        (void)detach_client(client);
        (void)destroy_client(client);
        return err;
    }

    generation_current = device_generation_current(generation);
    bool still_current = false;
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    still_current = s_service.client == client &&
                    generation_current && snapshot_current(change_epoch);
    if (still_current) {
        s_service.stats.client_running = true;
        s_service.stats.config_generation = generation;
        s_service.stats.rebuilds++;
    }
    xSemaphoreGive(s_service.lock);
    if (!still_current) {
        (void)detach_client(client);
        (void)destroy_client(client);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void update_config_stats(bool configured, uint32_t generation)
{
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.stats.configured = configured;
    s_service.stats.config_generation = generation;
    xSemaphoreGive(s_service.lock);
}

static void record_start_failure(void)
{
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.stats.start_failures++;
    xSemaphoreGive(s_service.lock);
}

static void cloud_service_task(void *arg)
{
    (void)arg;
    /* xTaskCreateWithCaps() can schedule this priority-5 worker before the
     * lower-priority start() caller gets to publish RUNNING. Blocking here is
     * therefore required: taskYIELD() only yields to peers of equal priority
     * and can starve both the caller and IDLE0 indefinitely. The same bit is
     * also set when startup fails, in which case service_stopping() below
     * makes the worker leave without touching partially-started resources. */
    (void)xEventGroupWaitBits(s_service.events,
                              CLOUD_SERVICE_START_RESOLVED_BIT,
                              pdFALSE, pdTRUE, portMAX_DELAY);

    for (;;) {
        if (service_stopping()) {
            break;
        }
        drain_change_queue();
        stop_active_client();

        uint32_t change_epoch = current_change_epoch();
        jetkvm_cloud_config_t *config =
            jetkvm_memory_policy_control_alloc(sizeof(*config));
        if (config) {
            memset(config, 0, sizeof(*config));
        }
        char device_id[JETKVM_DEVICE_ID_CAPACITY] = { 0 };
        uint32_t generation = 0U;
        esp_err_t err = config ?
            jetkvm_device_state_cloud_snapshot(config, &generation) :
            ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            err = jetkvm_device_state_get_id(device_id, sizeof(device_id));
        }
        /* Consume notifications already represented by this snapshot. A
         * notification racing after change_epoch was read still changes the
         * epoch, so freshness checks below cannot miss it. */
        drain_change_queue();
        bool configured = err == ESP_OK &&
                          jetkvm_cloud_config_is_registered(config);
        update_config_stats(configured, generation);

        bool retry = err != ESP_OK;
        if (err == ESP_OK && configured && snapshot_current(change_epoch)) {
            while (!jetkvm_time_sync_ready() &&
                   snapshot_current(change_epoch)) {
                esp_err_t wait_err = jetkvm_time_sync_wait(
                    pdMS_TO_TICKS(CLOUD_SERVICE_TIME_WAIT_MS));
                if (wait_err != ESP_OK && wait_err != ESP_ERR_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(CLOUD_SERVICE_TIME_WAIT_MS));
                }
            }
            if (jetkvm_time_sync_ready() &&
                snapshot_current(change_epoch)) {
                err = start_client_for_snapshot(
                    config, generation, change_epoch, device_id);
                if (err != ESP_OK && snapshot_current(change_epoch)) {
                    ESP_LOGW(TAG, "cloud client start failed: %s",
                             esp_err_to_name(err));
                    record_start_failure();
                    retry = true;
                } else {
                    retry = false;
                }
            }
        }
        if (config) {
            secure_zero(config, sizeof(*config));
            jetkvm_memory_policy_control_free(config);
        }
        secure_zero(device_id, sizeof(device_id));

        if (service_stopping()) {
            break;
        }
        if (!snapshot_current(change_epoch)) {
            continue;
        }
        TickType_t wait = retry ? pdMS_TO_TICKS(CLOUD_SERVICE_RETRY_MS) :
                                  portMAX_DELAY;
        uint8_t wake = 0U;
        (void)xQueueReceive(s_service.changes, &wake, wait);
    }

    stop_active_client();
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.task = NULL;
    xSemaphoreGive(s_service.lock);
    xEventGroupSetBits(s_service.events,
                       CLOUD_SERVICE_WORKER_STOPPED_BIT);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t ensure_resources(void)
{
    if (s_service.resources_ready) {
        return ESP_OK;
    }
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    QueueHandle_t changes = xQueueCreate(1U, sizeof(uint8_t));
    EventGroupHandle_t events = xEventGroupCreate();
    if (!lock || !changes || !events) {
        if (events) {
            vEventGroupDelete(events);
        }
        if (changes) {
            vQueueDelete(changes);
        }
        if (lock) {
            vSemaphoreDelete(lock);
        }
        return ESP_ERR_NO_MEM;
    }
    s_service.lock = lock;
    s_service.changes = changes;
    s_service.events = events;
    s_service.resources_ready = true;
    return ESP_OK;
}

static esp_err_t begin_start(jetkvm_oidc_validator_t *oidc)
{
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (service_lifecycle() != CLOUD_SERVICE_STOPPED) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* Clear reusable completion bits before STARTING becomes observable. A
     * concurrent stop() which observes STARTING can then safely block on the
     * start-resolved bit without consuming a stale signal from an older run. */
    drain_change_queue();
    xEventGroupClearBits(s_service.events,
                         CLOUD_SERVICE_WORKER_STOPPED_BIT |
                         CLOUD_SERVICE_STOP_COMPLETE_BIT |
                         CLOUD_SERVICE_START_RESOLVED_BIT);
    s_service.task = NULL;
    s_service.oidc = oidc;
    s_service.client = NULL;
    s_service.active_change_epoch = 0U;
    s_service.active_generation = 0U;
    secure_zero(s_service.active_identity,
                sizeof(s_service.active_identity));
    memset(&s_service.stats, 0, sizeof(s_service.stats));
    s_service.stats.started = true;
    atomic_store_explicit(&s_service.stop_requested, false,
                          memory_order_release);
    atomic_store_explicit(&s_service.lifecycle, CLOUD_SERVICE_STARTING,
                          memory_order_release);
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}

esp_err_t jetkvm_cloud_service_start(jetkvm_oidc_validator_t *oidc)
{
    if (!oidc) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_resources();
    if (err != ESP_OK) {
        return err;
    }
    err = begin_start(oidc);
    if (err != ESP_OK) {
        return err;
    }

    bool time_started = false;
    bool subscribed = false;
    err = jetkvm_time_sync_start();
    if (err == ESP_OK) {
        time_started = true;
        err = jetkvm_device_state_cloud_subscribe(
            config_changed, &s_service);
    }
    if (err == ESP_OK) {
        subscribed = true;
        BaseType_t created = xTaskCreateWithCaps(
            cloud_service_task, "jk_cloud_svc", CLOUD_SERVICE_TASK_STACK,
            NULL, CLOUD_SERVICE_TASK_PRIORITY, &s_service.task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        atomic_store_explicit(&s_service.lifecycle, CLOUD_SERVICE_RUNNING,
                              memory_order_release);
        xEventGroupSetBits(s_service.events,
                           CLOUD_SERVICE_START_RESOLVED_BIT);
        return ESP_OK;
    }

    if (subscribed) {
        jetkvm_device_state_cloud_unsubscribe(config_changed, &s_service);
    }
    if (time_started) {
        jetkvm_time_sync_stop();
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.oidc = NULL;
    s_service.stats.started = false;
    atomic_store_explicit(&s_service.lifecycle, CLOUD_SERVICE_STOPPED,
                          memory_order_release);
    xEventGroupSetBits(s_service.events,
                       CLOUD_SERVICE_START_RESOLVED_BIT);
    xSemaphoreGive(s_service.lock);
    return err;
}

esp_err_t jetkvm_cloud_service_stop(void)
{
    for (;;) {
        unsigned lifecycle = atomic_load_explicit(
            &s_service.lifecycle, memory_order_acquire);
        if (lifecycle == CLOUD_SERVICE_STOPPED) {
            return ESP_OK;
        }
        if (lifecycle == CLOUD_SERVICE_STARTING) {
            (void)xEventGroupWaitBits(s_service.events,
                                      CLOUD_SERVICE_START_RESOLVED_BIT,
                                      pdFALSE, pdTRUE, portMAX_DELAY);
            continue;
        }
        if (lifecycle == CLOUD_SERVICE_STOPPING) {
            (void)xEventGroupWaitBits(s_service.events,
                                      CLOUD_SERVICE_STOP_COMPLETE_BIT,
                                      pdFALSE, pdTRUE, portMAX_DELAY);
            return ESP_OK;
        }
        unsigned expected = CLOUD_SERVICE_RUNNING;
        if (atomic_compare_exchange_weak_explicit(
                &s_service.lifecycle, &expected, CLOUD_SERVICE_STOPPING,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }

    /* Unsubscribe first, then invalidate identity and wake the task. A callback
     * already captured by device_state remains safe because resources persist. */
    jetkvm_device_state_cloud_unsubscribe(config_changed, &s_service);
    atomic_store_explicit(&s_service.stop_requested, true,
                          memory_order_release);
    (void)atomic_fetch_add_explicit(&s_service.change_epoch, 1U,
                                    memory_order_acq_rel);
    uint8_t wake = 1U;
    (void)xQueueOverwrite(s_service.changes, &wake);
    xEventGroupWaitBits(s_service.events,
                        CLOUD_SERVICE_WORKER_STOPPED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    jetkvm_time_sync_stop();

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.oidc = NULL;
    s_service.stats.started = false;
    s_service.stats.client_running = false;
    atomic_store_explicit(&s_service.lifecycle, CLOUD_SERVICE_STOPPED,
                          memory_order_release);
    xEventGroupSetBits(s_service.events, CLOUD_SERVICE_STOP_COMPLETE_BIT);
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}

esp_err_t jetkvm_cloud_service_get_stats(
    jetkvm_cloud_service_stats_t *stats)
{
    if (!stats || !s_service.resources_ready) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_service_lifecycle_t lifecycle = service_lifecycle();
    if (lifecycle != CLOUD_SERVICE_RUNNING &&
        lifecycle != CLOUD_SERVICE_STOPPING) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    *stats = s_service.stats;
    stats->started = lifecycle == CLOUD_SERVICE_RUNNING;
    stats->time_ready = jetkvm_time_sync_ready();
    if (s_service.client) {
        (void)jetkvm_cloud_client_get_stats(s_service.client,
                                            &stats->client);
    }
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}
