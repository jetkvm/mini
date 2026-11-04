#define _POSIX_C_SOURCE 200809L

#include "host_fakes.h"

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fake_mutex {
    pthread_mutex_t mutex;
};

struct fake_queue {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    size_t item_size;
    bool full;
    unsigned char *item;
};

struct fake_events {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    EventBits_t bits;
};

#define FAKE_EVENT_BIT_COUNT 32U

static pthread_mutex_t s_event_waiter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_event_waiter_changed = PTHREAD_COND_INITIALIZER;
static unsigned s_event_waiters[FAKE_EVENT_BIT_COUNT];

static void update_event_waiters(EventBits_t bits, bool added)
{
    (void)pthread_mutex_lock(&s_event_waiter_lock);
    for (unsigned bit = 0U; bit < FAKE_EVENT_BIT_COUNT; ++bit) {
        if ((bits & ((EventBits_t)1U << bit)) != 0U) {
            if (added) {
                s_event_waiters[bit]++;
            } else if (s_event_waiters[bit] > 0U) {
                s_event_waiters[bit]--;
            }
        }
    }
    (void)pthread_cond_broadcast(&s_event_waiter_changed);
    (void)pthread_mutex_unlock(&s_event_waiter_lock);
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0) {
    }
}

static struct timespec deadline_ms(unsigned milliseconds)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(milliseconds / 1000U);
    deadline.tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_FAIL";
    }
}

static pthread_mutex_t s_memory_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_memory_allowed = true;
static unsigned s_memory_reserves;
static unsigned s_memory_releases;
static unsigned s_memory_active;

void *jetkvm_memory_policy_control_alloc(size_t size)
{
    return calloc(1U, size);
}

void jetkvm_memory_policy_control_free(void *memory)
{
    free(memory);
}

void fake_memory_reset(void)
{
    (void)pthread_mutex_lock(&s_memory_lock);
    s_memory_allowed = true;
    s_memory_reserves = 0U;
    s_memory_releases = 0U;
    s_memory_active = 0U;
    (void)pthread_mutex_unlock(&s_memory_lock);
}

void fake_memory_set_allowed(bool allowed)
{
    (void)pthread_mutex_lock(&s_memory_lock);
    s_memory_allowed = allowed;
    (void)pthread_mutex_unlock(&s_memory_lock);
}

bool jetkvm_memory_policy_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    if (!owner || strcmp(owner, "cloud-session-oidc") != 0 ||
        !reservation || reservation->active) {
        return false;
    }
    (void)pthread_mutex_lock(&s_memory_lock);
    s_memory_reserves++;
    bool allowed = s_memory_allowed;
    if (allowed) {
        reservation->generation = s_memory_reserves;
        reservation->active = true;
        s_memory_active++;
    }
    (void)pthread_mutex_unlock(&s_memory_lock);
    return allowed;
}

void jetkvm_memory_policy_release(
    jetkvm_memory_policy_reservation_t *reservation)
{
    if (!reservation || !reservation->active) {
        return;
    }
    (void)pthread_mutex_lock(&s_memory_lock);
    if (s_memory_active > 0U) {
        s_memory_active--;
        s_memory_releases++;
    }
    memset(reservation, 0, sizeof(*reservation));
    (void)pthread_mutex_unlock(&s_memory_lock);
}

unsigned fake_memory_reserve_count(void)
{
    (void)pthread_mutex_lock(&s_memory_lock);
    unsigned value = s_memory_reserves;
    (void)pthread_mutex_unlock(&s_memory_lock);
    return value;
}

unsigned fake_memory_release_count(void)
{
    (void)pthread_mutex_lock(&s_memory_lock);
    unsigned value = s_memory_releases;
    (void)pthread_mutex_unlock(&s_memory_lock);
    return value;
}

unsigned fake_memory_active_count(void)
{
    (void)pthread_mutex_lock(&s_memory_lock);
    unsigned value = s_memory_active;
    (void)pthread_mutex_unlock(&s_memory_lock);
    return value;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t semaphore = calloc(1U, sizeof(*semaphore));
    if (!semaphore || pthread_mutex_init(&semaphore->mutex, NULL) != 0) {
        free(semaphore);
        return NULL;
    }
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)timeout;
    return semaphore && pthread_mutex_lock(&semaphore->mutex) == 0 ?
           pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return semaphore && pthread_mutex_unlock(&semaphore->mutex) == 0 ?
           pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore) {
        (void)pthread_mutex_destroy(&semaphore->mutex);
        free(semaphore);
    }
}

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    (void)length;
    QueueHandle_t queue = calloc(1U, sizeof(*queue));
    if (!queue) {
        return NULL;
    }
    queue->item = calloc(1U, item_size);
    queue->item_size = item_size;
    if (!queue->item || pthread_mutex_init(&queue->mutex, NULL) != 0 ||
        pthread_cond_init(&queue->changed, NULL) != 0) {
        free(queue->item);
        free(queue);
        return NULL;
    }
    return queue;
}

BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    if (!queue || !item) {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    memcpy(queue->item, item, queue->item_size);
    queue->full = true;
    (void)pthread_cond_broadcast(&queue->changed);
    (void)pthread_mutex_unlock(&queue->mutex);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    if (!queue || !item) {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    if (!queue->full && timeout == portMAX_DELAY) {
        while (!queue->full) {
            (void)pthread_cond_wait(&queue->changed, &queue->mutex);
        }
    } else if (!queue->full && timeout != 0U) {
        unsigned wait_ms = timeout > 20U ? 20U : timeout;
        struct timespec deadline = deadline_ms(wait_ms);
        while (!queue->full &&
               pthread_cond_timedwait(&queue->changed, &queue->mutex,
                                      &deadline) == 0) {
        }
    }
    BaseType_t result = pdFALSE;
    if (queue->full) {
        memcpy(item, queue->item, queue->item_size);
        queue->full = false;
        result = pdTRUE;
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    return result;
}

void vQueueDelete(QueueHandle_t queue)
{
    if (queue) {
        (void)pthread_cond_destroy(&queue->changed);
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue->item);
        free(queue);
    }
}

EventGroupHandle_t xEventGroupCreate(void)
{
    EventGroupHandle_t events = calloc(1U, sizeof(*events));
    if (!events || pthread_mutex_init(&events->mutex, NULL) != 0 ||
        pthread_cond_init(&events->changed, NULL) != 0) {
        free(events);
        return NULL;
    }
    return events;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t events, EventBits_t bits)
{
    (void)pthread_mutex_lock(&events->mutex);
    events->bits |= bits;
    EventBits_t result = events->bits;
    (void)pthread_cond_broadcast(&events->changed);
    (void)pthread_mutex_unlock(&events->mutex);
    return result;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t events, EventBits_t bits)
{
    (void)pthread_mutex_lock(&events->mutex);
    EventBits_t previous = events->bits;
    events->bits &= ~bits;
    (void)pthread_mutex_unlock(&events->mutex);
    return previous;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t events,
                                EventBits_t bits, BaseType_t clear_on_exit,
                                BaseType_t wait_for_all, TickType_t timeout)
{
    (void)timeout;
    (void)pthread_mutex_lock(&events->mutex);
    bool waiter_announced = false;
    for (;;) {
        EventBits_t matched = events->bits & bits;
        if ((wait_for_all && matched == bits) ||
            (!wait_for_all && matched != 0U)) {
            EventBits_t result = events->bits;
            if (clear_on_exit) {
                events->bits &= ~bits;
            }
            (void)pthread_mutex_unlock(&events->mutex);
            if (waiter_announced) {
                update_event_waiters(bits, false);
            }
            return result;
        }
        if (!waiter_announced) {
            update_event_waiters(bits, true);
            waiter_announced = true;
        }
        (void)pthread_cond_wait(&events->changed, &events->mutex);
    }
}

bool fake_event_wait_for_waiters(EventBits_t bits, unsigned minimum,
                                 unsigned timeout_ms)
{
    struct timespec deadline = deadline_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_event_waiter_lock);
    for (;;) {
        unsigned count = UINT32_MAX;
        bool selected = false;
        for (unsigned bit = 0U; bit < FAKE_EVENT_BIT_COUNT; ++bit) {
            if ((bits & ((EventBits_t)1U << bit)) != 0U) {
                if (!selected || s_event_waiters[bit] < count) {
                    count = s_event_waiters[bit];
                }
                selected = true;
            }
        }
        if (selected && count >= minimum) {
            (void)pthread_mutex_unlock(&s_event_waiter_lock);
            return true;
        }
        if (pthread_cond_timedwait(&s_event_waiter_changed,
                                   &s_event_waiter_lock, &deadline) != 0) {
            (void)pthread_mutex_unlock(&s_event_waiter_lock);
            return false;
        }
    }
}

void vEventGroupDelete(EventGroupHandle_t events)
{
    if (events) {
        (void)pthread_cond_destroy(&events->changed);
        (void)pthread_mutex_destroy(&events->mutex);
        free(events);
    }
}

typedef struct {
    TaskFunction_t function;
    void *argument;
} task_launch_t;

static pthread_mutex_t s_task_create_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_task_create_changed = PTHREAD_COND_INITIALIZER;
static bool s_block_next_task_create;
static bool s_fail_next_task_create;
static bool s_task_create_entered;
static bool s_release_task_create;

static void *task_entry(void *opaque)
{
    task_launch_t launch = *(task_launch_t *)opaque;
    free(opaque);
    launch.function(launch.argument);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t function, const char *name,
                       uint32_t stack_size, void *argument,
                       unsigned priority, TaskHandle_t *task)
{
    (void)name;
    (void)stack_size;
    (void)priority;
    (void)pthread_mutex_lock(&s_task_create_lock);
    bool block_return = s_block_next_task_create;
    bool fail_create = s_fail_next_task_create;
    s_block_next_task_create = false;
    s_fail_next_task_create = false;
    (void)pthread_mutex_unlock(&s_task_create_lock);

    if (!fail_create) {
        task_launch_t *launch = malloc(sizeof(*launch));
        if (!launch) {
            return pdFALSE;
        }
        launch->function = function;
        launch->argument = argument;
        pthread_t thread;
        if (pthread_create(&thread, NULL, task_entry, launch) != 0) {
            free(launch);
            return pdFALSE;
        }
        (void)pthread_detach(thread);
    }

    if (block_return) {
        (void)pthread_mutex_lock(&s_task_create_lock);
        s_task_create_entered = true;
        (void)pthread_cond_broadcast(&s_task_create_changed);
        while (!s_release_task_create) {
            (void)pthread_cond_wait(&s_task_create_changed,
                                    &s_task_create_lock);
        }
        (void)pthread_mutex_unlock(&s_task_create_lock);
    }
    if (fail_create) {
        return pdFALSE;
    }
    if (task) {
        *task = (TaskHandle_t)(uintptr_t)1U;
    }
    return pdPASS;
}

void fake_task_block_next_create(bool fail)
{
    (void)pthread_mutex_lock(&s_task_create_lock);
    s_block_next_task_create = true;
    s_fail_next_task_create = fail;
    s_task_create_entered = false;
    s_release_task_create = false;
    (void)pthread_mutex_unlock(&s_task_create_lock);
}

bool fake_task_wait_create_entered(unsigned timeout_ms)
{
    struct timespec deadline = deadline_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_task_create_lock);
    while (!s_task_create_entered) {
        if (pthread_cond_timedwait(&s_task_create_changed,
                                   &s_task_create_lock, &deadline) != 0) {
            break;
        }
    }
    bool entered = s_task_create_entered;
    (void)pthread_mutex_unlock(&s_task_create_lock);
    return entered;
}

void fake_task_release_create(void)
{
    (void)pthread_mutex_lock(&s_task_create_lock);
    s_release_task_create = true;
    (void)pthread_cond_broadcast(&s_task_create_changed);
    (void)pthread_mutex_unlock(&s_task_create_lock);
}

void vTaskDelete(TaskHandle_t task)
{
    if (!task) {
        pthread_exit(NULL);
    }
}

void vTaskDelay(TickType_t delay)
{
    sleep_ms(delay > 5U ? 5U : delay);
}

void fake_task_yield(void)
{
    (void)sched_yield();
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = { .version = "test" };
    return &description;
}

void fake_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

static pthread_mutex_t s_state_lock = PTHREAD_MUTEX_INITIALIZER;
static jetkvm_cloud_config_t s_config;
static uint32_t s_generation = 1U;
static jetkvm_device_cloud_change_cb_t s_observer;
static void *s_observer_ctx;
static jetkvm_device_cloud_change_cb_t s_late_observer;
static void *s_late_observer_ctx;

static void copy_string(char *output, size_t capacity, const char *value)
{
    (void)snprintf(output, capacity, "%s", value ? value : "");
}

bool jetkvm_cloud_config_is_registered(const jetkvm_cloud_config_t *config)
{
    return config && config->cloud_url[0] != '\0' &&
           config->cloud_token[0] != '\0' &&
           config->google_identity[0] != '\0';
}

void fake_state_reset(bool registered, const char *token,
                      const char *identity)
{
    (void)pthread_mutex_lock(&s_state_lock);
    memset(&s_config, 0, sizeof(s_config));
    s_config.schema_version = 1U;
    copy_string(s_config.cloud_url, sizeof(s_config.cloud_url),
                "https://api.jetkvm.com");
    copy_string(s_config.cloud_app_url, sizeof(s_config.cloud_app_url),
                "https://app.jetkvm.com");
    if (registered) {
        copy_string(s_config.cloud_token, sizeof(s_config.cloud_token), token);
        copy_string(s_config.google_identity,
                    sizeof(s_config.google_identity), identity);
    }
    s_generation = 1U;
    s_observer = NULL;
    s_observer_ctx = NULL;
    s_late_observer = NULL;
    s_late_observer_ctx = NULL;
    (void)pthread_mutex_unlock(&s_state_lock);
}

uint32_t fake_state_replace(bool registered, const char *token,
                            const char *identity, bool notify)
{
    jetkvm_device_cloud_change_cb_t observer = NULL;
    void *ctx = NULL;
    uint32_t generation = 0U;
    (void)pthread_mutex_lock(&s_state_lock);
    memset(s_config.cloud_token, 0, sizeof(s_config.cloud_token));
    memset(s_config.google_identity, 0, sizeof(s_config.google_identity));
    if (registered) {
        copy_string(s_config.cloud_token, sizeof(s_config.cloud_token), token);
        copy_string(s_config.google_identity,
                    sizeof(s_config.google_identity), identity);
    }
    s_generation = s_generation == UINT32_MAX ? 1U : s_generation + 1U;
    generation = s_generation;
    if (notify) {
        observer = s_observer;
        ctx = s_observer_ctx;
    }
    (void)pthread_mutex_unlock(&s_state_lock);
    if (observer) {
        observer(4U, generation, ctx);
    }
    return generation;
}

void fake_state_emit_current(void)
{
    jetkvm_device_cloud_change_cb_t observer = NULL;
    void *ctx = NULL;
    uint32_t generation = 0U;
    (void)pthread_mutex_lock(&s_state_lock);
    observer = s_observer;
    ctx = s_observer_ctx;
    generation = s_generation;
    (void)pthread_mutex_unlock(&s_state_lock);
    if (observer) {
        observer(4U, generation, ctx);
    }
}

void fake_state_emit_late(void)
{
    jetkvm_device_cloud_change_cb_t observer = NULL;
    void *ctx = NULL;
    uint32_t generation = 0U;
    (void)pthread_mutex_lock(&s_state_lock);
    observer = s_late_observer;
    ctx = s_late_observer_ctx;
    generation = s_generation;
    (void)pthread_mutex_unlock(&s_state_lock);
    if (observer) {
        observer(4U, generation, ctx);
    }
}

esp_err_t jetkvm_device_state_cloud_snapshot(
    jetkvm_cloud_config_t *config, uint32_t *generation)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_state_lock);
    *config = s_config;
    if (generation) {
        *generation = s_generation;
    }
    (void)pthread_mutex_unlock(&s_state_lock);
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_public_snapshot(
    jetkvm_device_cloud_public_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_state_lock);
    memset(state, 0, sizeof(*state));
    state->registered = jetkvm_cloud_config_is_registered(&s_config);
    state->generation = s_generation;
    memcpy(state->cloud_url, s_config.cloud_url, sizeof(state->cloud_url));
    memcpy(state->cloud_app_url, s_config.cloud_app_url,
           sizeof(state->cloud_app_url));
    (void)pthread_mutex_unlock(&s_state_lock);
    return ESP_OK;
}

esp_err_t jetkvm_device_state_get_id(char *output, size_t output_capacity)
{
    if (!output || output_capacity < JETKVM_DEVICE_ID_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    copy_string(output, output_capacity, "001122aabbcc");
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_subscribe(
    jetkvm_device_cloud_change_cb_t callback, void *ctx)
{
    (void)pthread_mutex_lock(&s_state_lock);
    if (s_observer && (s_observer != callback || s_observer_ctx != ctx)) {
        (void)pthread_mutex_unlock(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_observer = callback;
    s_observer_ctx = ctx;
    s_late_observer = callback;
    s_late_observer_ctx = ctx;
    (void)pthread_mutex_unlock(&s_state_lock);
    return ESP_OK;
}

void jetkvm_device_state_cloud_unsubscribe(
    jetkvm_device_cloud_change_cb_t callback, void *ctx)
{
    (void)pthread_mutex_lock(&s_state_lock);
    if (s_observer == callback && s_observer_ctx == ctx) {
        s_observer = NULL;
        s_observer_ctx = NULL;
    }
    (void)pthread_mutex_unlock(&s_state_lock);
}

static pthread_mutex_t s_time_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_time_started;
static bool s_time_ready;

void fake_time_set_ready(bool ready)
{
    (void)pthread_mutex_lock(&s_time_lock);
    s_time_ready = ready;
    (void)pthread_mutex_unlock(&s_time_lock);
}

esp_err_t jetkvm_time_sync_start(void)
{
    (void)pthread_mutex_lock(&s_time_lock);
    s_time_started = true;
    (void)pthread_mutex_unlock(&s_time_lock);
    return ESP_OK;
}

esp_err_t jetkvm_time_sync_wait(TickType_t timeout)
{
    (void)timeout;
    sleep_ms(1U);
    return jetkvm_time_sync_ready() ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool jetkvm_time_sync_ready(void)
{
    (void)pthread_mutex_lock(&s_time_lock);
    bool ready = s_time_started && s_time_ready;
    (void)pthread_mutex_unlock(&s_time_lock);
    return ready;
}

void jetkvm_time_sync_stop(void)
{
    (void)pthread_mutex_lock(&s_time_lock);
    s_time_started = false;
    (void)pthread_mutex_unlock(&s_time_lock);
}

jetkvm_oidc_status_t jetkvm_oidc_validate_session(
    jetkvm_oidc_validator_t *validator, const char *id_token,
    char *identity, size_t identity_capacity)
{
    if (validator && id_token &&
        strcmp(id_token, "jwks-unavailable") == 0) {
        return JETKVM_OIDC_JWKS_UNAVAILABLE;
    }
    if (!validator || !id_token || strncmp(id_token, "ok:", 3U) != 0) {
        return JETKVM_OIDC_INVALID;
    }
    size_t length = strlen(id_token + 3U);
    if (length + 1U > identity_capacity) {
        return JETKVM_OIDC_IDENTITY_TOO_LARGE;
    }
    memcpy(identity, id_token + 3U, length + 1U);
    return JETKVM_OIDC_OK;
}

const char *jetkvm_oidc_status_name(jetkvm_oidc_status_t status)
{
    return status == JETKVM_OIDC_OK ? "ok" : "invalid";
}

struct jetkvm_cloud_client {
    char token[128];
    jetkvm_cloud_identity_validator_t validate_identity;
    void *identity_ctx;
    jetkvm_cloud_client_stats_t stats;
};

static pthread_mutex_t s_client_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_client_changed = PTHREAD_COND_INITIALIZER;
static struct jetkvm_cloud_client *s_current_client;
static jetkvm_cloud_identity_validator_t s_last_validator;
static void *s_last_identity_ctx;
static unsigned s_start_count;
static unsigned s_destroy_count;
static bool s_block_next_start;
static bool s_start_entered;
static bool s_release_start;
static char s_last_token[128];

void fake_client_reset(void)
{
    (void)pthread_mutex_lock(&s_client_lock);
    s_current_client = NULL;
    s_last_validator = NULL;
    s_last_identity_ctx = NULL;
    s_start_count = 0U;
    s_destroy_count = 0U;
    s_block_next_start = false;
    s_start_entered = false;
    s_release_start = false;
    memset(s_last_token, 0, sizeof(s_last_token));
    (void)pthread_mutex_unlock(&s_client_lock);
}

esp_err_t jetkvm_cloud_client_init(
    const jetkvm_cloud_client_config_t *config,
    jetkvm_cloud_client_handle_t *client_out)
{
    if (!config || !client_out || !config->validate_identity) {
        return ESP_ERR_INVALID_ARG;
    }
    struct jetkvm_cloud_client *client = calloc(1U, sizeof(*client));
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    copy_string(client->token, sizeof(client->token), config->token);
    client->validate_identity = config->validate_identity;
    client->identity_ctx = config->identity_ctx;
    *client_out = client;
    return ESP_OK;
}

esp_err_t jetkvm_cloud_client_start(jetkvm_cloud_client_handle_t client)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_client_lock);
    s_start_count++;
    s_current_client = client;
    s_last_validator = client->validate_identity;
    s_last_identity_ctx = client->identity_ctx;
    copy_string(s_last_token, sizeof(s_last_token), client->token);
    client->stats.started = true;
    client->stats.state = JETKVM_CLOUD_CLIENT_CONNECTED;
    client->stats.connected = true;
    bool block = s_block_next_start;
    s_block_next_start = false;
    if (block) {
        s_start_entered = true;
        (void)pthread_cond_broadcast(&s_client_changed);
        while (!s_release_start) {
            (void)pthread_cond_wait(&s_client_changed, &s_client_lock);
        }
        s_release_start = false;
        s_start_entered = false;
    }
    (void)pthread_cond_broadcast(&s_client_changed);
    (void)pthread_mutex_unlock(&s_client_lock);
    return ESP_OK;
}

esp_err_t jetkvm_cloud_client_stop(jetkvm_cloud_client_handle_t client)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    client->stats.started = false;
    client->stats.state = JETKVM_CLOUD_CLIENT_STOPPED;
    return ESP_OK;
}

esp_err_t jetkvm_cloud_client_destroy(jetkvm_cloud_client_handle_t client)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_client_lock);
    if (s_current_client == client) {
        s_current_client = NULL;
    }
    s_destroy_count++;
    (void)pthread_cond_broadcast(&s_client_changed);
    (void)pthread_mutex_unlock(&s_client_lock);
    free(client);
    return ESP_OK;
}

esp_err_t jetkvm_cloud_client_get_stats(
    jetkvm_cloud_client_handle_t client,
    jetkvm_cloud_client_stats_t *stats)
{
    if (!client || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    *stats = client->stats;
    return ESP_OK;
}

void fake_client_block_next_start(void)
{
    (void)pthread_mutex_lock(&s_client_lock);
    s_block_next_start = true;
    s_start_entered = false;
    s_release_start = false;
    (void)pthread_mutex_unlock(&s_client_lock);
}

bool fake_client_wait_start_entered(unsigned timeout_ms)
{
    struct timespec deadline = deadline_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_client_lock);
    while (!s_start_entered) {
        if (pthread_cond_timedwait(&s_client_changed, &s_client_lock,
                                   &deadline) != 0) {
            break;
        }
    }
    bool entered = s_start_entered;
    (void)pthread_mutex_unlock(&s_client_lock);
    return entered;
}

void fake_client_release_start(void)
{
    (void)pthread_mutex_lock(&s_client_lock);
    s_release_start = true;
    (void)pthread_cond_broadcast(&s_client_changed);
    (void)pthread_mutex_unlock(&s_client_lock);
}

bool fake_client_wait_counts(unsigned starts, unsigned destroys,
                             unsigned timeout_ms)
{
    struct timespec deadline = deadline_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_client_lock);
    while (s_start_count < starts || s_destroy_count < destroys) {
        if (pthread_cond_timedwait(&s_client_changed, &s_client_lock,
                                   &deadline) != 0) {
            break;
        }
    }
    bool reached = s_start_count >= starts && s_destroy_count >= destroys;
    (void)pthread_mutex_unlock(&s_client_lock);
    return reached;
}

unsigned fake_client_start_count(void)
{
    (void)pthread_mutex_lock(&s_client_lock);
    unsigned count = s_start_count;
    (void)pthread_mutex_unlock(&s_client_lock);
    return count;
}

unsigned fake_client_destroy_count(void)
{
    (void)pthread_mutex_lock(&s_client_lock);
    unsigned count = s_destroy_count;
    (void)pthread_mutex_unlock(&s_client_lock);
    return count;
}

bool fake_client_last_token_is(const char *token)
{
    (void)pthread_mutex_lock(&s_client_lock);
    bool equal = token && strcmp(s_last_token, token) == 0;
    (void)pthread_mutex_unlock(&s_client_lock);
    return equal;
}

jetkvm_cloud_identity_validation_t fake_client_validate_token_status(
    const char *token, size_t token_length)
{
    (void)pthread_mutex_lock(&s_client_lock);
    jetkvm_cloud_identity_validator_t validator = s_last_validator;
    void *ctx = s_last_identity_ctx;
    (void)pthread_mutex_unlock(&s_client_lock);
    return validator ? validator(token, token_length, ctx) :
                       JETKVM_CLOUD_IDENTITY_INVALID;
}

bool fake_client_validate_token(const char *token, size_t token_length)
{
    return fake_client_validate_token_status(token, token_length) ==
           JETKVM_CLOUD_IDENTITY_VALID;
}
