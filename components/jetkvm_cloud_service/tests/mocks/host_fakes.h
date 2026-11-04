/* Host-only ESP-IDF/FreeRTOS and cloud dependency fakes. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x105

const char *esp_err_to_name(esp_err_t error);

typedef struct {
    uint32_t generation;
    uint8_t slot;
    bool active;
} jetkvm_memory_policy_reservation_t;
bool jetkvm_memory_policy_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation);
void jetkvm_memory_policy_release(
    jetkvm_memory_policy_reservation_t *reservation);
void *jetkvm_memory_policy_control_alloc(size_t size);
void jetkvm_memory_policy_control_free(void *memory);

typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef int BaseType_t;
typedef struct fake_mutex *SemaphoreHandle_t;
typedef struct fake_queue *QueueHandle_t;
typedef struct fake_events *EventGroupHandle_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#define BIT0 (1U << 0)
#define BIT1 (1U << 1)
#define BIT2 (1U << 2)

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t timeout);
void vQueueDelete(QueueHandle_t queue);

EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t events, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t events, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t events,
                                EventBits_t bits, BaseType_t clear_on_exit,
                                BaseType_t wait_for_all, TickType_t timeout);
void vEventGroupDelete(EventGroupHandle_t events);

BaseType_t xTaskCreate(TaskFunction_t function, const char *name,
                       uint32_t stack_size, void *argument,
                       unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t delay);
void fake_task_yield(void);
#define taskYIELD() fake_task_yield()
void fake_task_block_next_create(bool fail);
bool fake_task_wait_create_entered(unsigned timeout_ms);
void fake_task_release_create(void);
bool fake_event_wait_for_waiters(EventBits_t bits, unsigned minimum,
                                 unsigned timeout_ms);

typedef struct {
    char version[32];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
void fake_log(const char *tag, const char *format, ...);
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#define ESP_LOGE(...) fake_log(__VA_ARGS__)

#define JETKVM_CLOUD_URL_CAPACITY 513U
#define JETKVM_CLOUD_APP_URL_CAPACITY 513U
#define JETKVM_CLOUD_TOKEN_CAPACITY 1025U
#define JETKVM_CLOUD_IDENTITY_CAPACITY 384U
#define JETKVM_DEVICE_ID_CAPACITY 13U
#define JETKVM_OIDC_MAX_TOKEN_BYTES 16384U

typedef struct {
    uint32_t schema_version;
    char cloud_url[JETKVM_CLOUD_URL_CAPACITY];
    char cloud_app_url[JETKVM_CLOUD_APP_URL_CAPACITY];
    char cloud_token[JETKVM_CLOUD_TOKEN_CAPACITY];
    char google_identity[JETKVM_CLOUD_IDENTITY_CAPACITY];
} jetkvm_cloud_config_t;

typedef void (*jetkvm_device_cloud_change_cb_t)(
    uint32_t changes, uint32_t generation, void *ctx);
typedef struct {
    bool registered;
    uint32_t generation;
    char cloud_url[JETKVM_CLOUD_URL_CAPACITY];
    char cloud_app_url[JETKVM_CLOUD_APP_URL_CAPACITY];
} jetkvm_device_cloud_public_state_t;

bool jetkvm_cloud_config_is_registered(const jetkvm_cloud_config_t *config);
esp_err_t jetkvm_device_state_cloud_snapshot(
    jetkvm_cloud_config_t *config, uint32_t *generation);
esp_err_t jetkvm_device_state_cloud_public_snapshot(
    jetkvm_device_cloud_public_state_t *state);
esp_err_t jetkvm_device_state_get_id(char *output, size_t output_capacity);
esp_err_t jetkvm_device_state_cloud_subscribe(
    jetkvm_device_cloud_change_cb_t callback, void *ctx);
void jetkvm_device_state_cloud_unsubscribe(
    jetkvm_device_cloud_change_cb_t callback, void *ctx);

typedef struct jetkvm_oidc_validator {
    unsigned marker;
} jetkvm_oidc_validator_t;
typedef enum {
    JETKVM_OIDC_OK = 0,
    JETKVM_OIDC_INVALID,
    JETKVM_OIDC_IDENTITY_TOO_LARGE,
    JETKVM_OIDC_INVALID_JWKS,
    JETKVM_OIDC_JWKS_UNAVAILABLE,
    JETKVM_OIDC_CLOCK_NOT_READY,
    JETKVM_OIDC_BUSY,
    JETKVM_OIDC_NO_MEMORY,
    JETKVM_OIDC_CRYPTO_ERROR,
} jetkvm_oidc_status_t;
jetkvm_oidc_status_t jetkvm_oidc_validate_session(
    jetkvm_oidc_validator_t *validator, const char *id_token,
    char *identity, size_t identity_capacity);
const char *jetkvm_oidc_status_name(jetkvm_oidc_status_t status);

typedef struct jetkvm_cloud_client *jetkvm_cloud_client_handle_t;
typedef enum {
    JETKVM_CLOUD_IDENTITY_INVALID = 0,
    JETKVM_CLOUD_IDENTITY_VALID,
    JETKVM_CLOUD_IDENTITY_RETRYABLE,
} jetkvm_cloud_identity_validation_t;
typedef jetkvm_cloud_identity_validation_t
(*jetkvm_cloud_identity_validator_t)(
    const char *oidc_google, size_t token_length, void *ctx);
typedef enum {
    JETKVM_CLOUD_CLIENT_STOPPED = 0,
    JETKVM_CLOUD_CLIENT_CONNECTING,
    JETKVM_CLOUD_CLIENT_CONNECTED,
    JETKVM_CLOUD_CLIENT_RECONNECTING,
} jetkvm_cloud_client_state_t;
typedef struct {
    jetkvm_cloud_client_state_t state;
    bool started;
    bool connected;
    bool session_active;
    uint32_t connection_epoch;
    uint32_t session_id;
    uint32_t generation;
    uint64_t connections;
    uint64_t disconnections;
    uint64_t reconnect_requests;
    uint64_t websocket_errors;
    uint64_t pongs_received;
    uint64_t rx_messages;
    uint64_t rx_bytes;
    uint64_t rx_dropped;
    uint64_t rx_protocol_errors;
    uint64_t rx_oversize;
    uint64_t offers_received;
    uint64_t offers_accepted;
    uint64_t offers_rejected_identity;
    uint64_t candidates_received;
    uint64_t candidates_accepted;
    uint64_t signaling_errors;
    uint64_t tx_messages;
    uint64_t tx_bytes;
    uint64_t tx_dropped;
    uint64_t tx_send_errors;
} jetkvm_cloud_client_stats_t;
typedef struct {
    const char *url;
    const char *device_id;
    const char *app_version;
    const char *sku;
    const char *token;
    jetkvm_cloud_identity_validator_t validate_identity;
    void *identity_ctx;
    uint32_t pong_timeout_sec;
    uint32_t reconnect_timeout_ms;
} jetkvm_cloud_client_config_t;

esp_err_t jetkvm_cloud_client_init(
    const jetkvm_cloud_client_config_t *config,
    jetkvm_cloud_client_handle_t *client);
esp_err_t jetkvm_cloud_client_start(jetkvm_cloud_client_handle_t client);
esp_err_t jetkvm_cloud_client_stop(jetkvm_cloud_client_handle_t client);
esp_err_t jetkvm_cloud_client_destroy(jetkvm_cloud_client_handle_t client);
esp_err_t jetkvm_cloud_client_get_stats(
    jetkvm_cloud_client_handle_t client,
    jetkvm_cloud_client_stats_t *stats);

typedef struct {
    bool started;
    bool configured;
    bool time_ready;
    bool client_running;
    uint32_t config_generation;
    uint64_t rebuilds;
    uint64_t start_failures;
    uint64_t destroy_failures;
    jetkvm_cloud_client_stats_t client;
} jetkvm_cloud_service_stats_t;
esp_err_t jetkvm_cloud_service_start(jetkvm_oidc_validator_t *oidc);
esp_err_t jetkvm_cloud_service_stop(void);
esp_err_t jetkvm_cloud_service_get_stats(
    jetkvm_cloud_service_stats_t *stats);

esp_err_t jetkvm_time_sync_start(void);
esp_err_t jetkvm_time_sync_wait(TickType_t timeout);
bool jetkvm_time_sync_ready(void);
void jetkvm_time_sync_stop(void);

void fake_state_reset(bool registered, const char *token,
                      const char *identity);
uint32_t fake_state_replace(bool registered, const char *token,
                            const char *identity, bool notify);
void fake_state_emit_current(void);
void fake_state_emit_late(void);
void fake_time_set_ready(bool ready);
void fake_client_reset(void);
void fake_client_block_next_start(void);
bool fake_client_wait_start_entered(unsigned timeout_ms);
void fake_client_release_start(void);
bool fake_client_wait_counts(unsigned starts, unsigned destroys,
                             unsigned timeout_ms);
unsigned fake_client_start_count(void);
unsigned fake_client_destroy_count(void);
bool fake_client_last_token_is(const char *token);
bool fake_client_validate_token(const char *token, size_t token_length);
jetkvm_cloud_identity_validation_t fake_client_validate_token_status(
    const char *token, size_t token_length);
void fake_memory_reset(void);
void fake_memory_set_allowed(bool allowed);
unsigned fake_memory_reserve_count(void);
unsigned fake_memory_release_count(void);
unsigned fake_memory_active_count(void);
