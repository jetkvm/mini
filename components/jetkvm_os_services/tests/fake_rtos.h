/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* Threaded stand-ins for the ESP-IDF, FreeRTOS, PSA and memory policy calls
 * of os_services_relay.c: a task is a thread, a mutex a pthread mutex and a
 * task notification a counter under a condition variable. run_host_tests.sh
 * serves every such header that file includes from this one; the test
 * defines the HTTP client, OTA and session calls. */

#pragma once

#include <errno.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101

#define EXT_RAM_BSS_ATTR

static inline void fake_log(const char *tag, const char *format, ...) {}
#define ESP_LOGE fake_log
#define ESP_LOGW fake_log
#define ESP_LOGI fake_log

#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
static inline void *heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{
    return calloc(count, size);
}
static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    return malloc(size);
}
static inline void heap_caps_free(void *memory)
{
    free(memory);
}

/* The test moves the clock on by this much. */
static int64_t fake_time_skipped_us;
static inline int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000 +
           __atomic_load_n(&fake_time_skipped_us, __ATOMIC_ACQUIRE);
}

/* --- FreeRTOS ------------------------------------------------------------ */

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef struct fake_task {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t count;
} *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

/* The calling thread's task, made on first use. */
static inline TaskHandle_t fake_task_self(void)
{
    static _Thread_local struct fake_task task = {
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0U};
    return &task;
}

static inline void xTaskNotifyGive(TaskHandle_t task)
{
    pthread_mutex_lock(&task->mutex);
    task->count++;
    pthread_cond_signal(&task->cond);
    pthread_mutex_unlock(&task->mutex);
}

static inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait_ms)
{
    TaskHandle_t task = fake_task_self();
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += wait_ms / 1000U;
    until.tv_nsec += (long)(wait_ms % 1000U) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&task->mutex);
    while (task->count == 0U &&
           pthread_cond_timedwait(&task->cond, &task->mutex, &until) != ETIMEDOUT) {
    }
    const uint32_t count = task->count;
    task->count = clear ? 0U : count - (count > 0U);
    pthread_mutex_unlock(&task->mutex);
    return count;
}

/* The test runs the worker's jobs on its own threads. */
static inline BaseType_t xTaskCreateWithCaps(TaskFunction_t entry,
                                             const char *name, uint32_t stack,
                                             void *arg, UBaseType_t priority,
                                             TaskHandle_t *task, uint32_t caps)
{
    return pdFAIL;
}
static inline void vTaskDeleteWithCaps(TaskHandle_t task) {}
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    return 0U;
}

typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    return &mutex;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait)
{
    return pthread_mutex_lock(mutex) == 0 ? pdTRUE : pdFALSE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    return pthread_mutex_unlock(mutex) == 0 ? pdTRUE : pdFALSE;
}

/* --- PSA hashes, by OpenSSL ---------------------------------------------- */

typedef int psa_status_t;
typedef int psa_algorithm_t;
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_1 1
#define PSA_ALG_SHA_256 256
#define PSA_ALG_SHA_384 384
#define PSA_ALG_SHA_512 512
typedef struct {
    EVP_MD_CTX *ctx;
} psa_hash_operation_t;

static inline psa_status_t psa_hash_setup(psa_hash_operation_t *hash,
                                          psa_algorithm_t algorithm)
{
    hash->ctx = EVP_MD_CTX_new();
    const EVP_MD *md = algorithm == PSA_ALG_SHA_1 ? EVP_sha1() :
                       algorithm == PSA_ALG_SHA_256 ? EVP_sha256() :
                       algorithm == PSA_ALG_SHA_384 ? EVP_sha384() :
                                                      EVP_sha512();
    return EVP_DigestInit_ex(hash->ctx, md, NULL) == 1 ? PSA_SUCCESS : -1;
}
static inline psa_status_t psa_hash_update(psa_hash_operation_t *hash,
                                           const uint8_t *data, size_t length)
{
    return EVP_DigestUpdate(hash->ctx, data, length) == 1 ? PSA_SUCCESS : -1;
}
static inline psa_status_t psa_hash_finish(psa_hash_operation_t *hash,
                                           uint8_t *digest, size_t capacity,
                                           size_t *length)
{
    unsigned int n = 0U;
    const bool ok = (size_t)EVP_MD_CTX_get_size(hash->ctx) <= capacity &&
                    EVP_DigestFinal_ex(hash->ctx, digest, &n) == 1;
    *length = n;
    return ok ? PSA_SUCCESS : -1;
}
static inline psa_status_t psa_hash_abort(psa_hash_operation_t *hash)
{
    EVP_MD_CTX_free(hash->ctx);
    hash->ctx = NULL;
    return PSA_SUCCESS;
}

/* --- The memory policy admits everything --------------------------------- */

typedef struct {
    bool active;
} jetkvm_memory_policy_reservation_t;
static inline bool jetkvm_memory_policy_reserve_psram(
    const char *owner, size_t bytes,
    jetkvm_memory_policy_reservation_t *reservation)
{
    return true;
}
static inline bool jetkvm_memory_policy_wait_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    return true;
}
static inline bool jetkvm_memory_policy_wait_reserve_download(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    return true;
}
static inline void jetkvm_memory_policy_release(
    jetkvm_memory_policy_reservation_t *reservation)
{
}

/* --- HTTP client: the test defines the calls ----------------------------- */

typedef struct fake_http *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ON_HEADER } esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    char *header_key;
    char *header_value;
    void *user_data;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int buffer_size;
    int buffer_size_tx;
    esp_err_t (*event_handler)(esp_http_client_event_t *event);
    void *user_data;
} esp_http_client_config_t;
static inline esp_err_t esp_crt_bundle_attach(void *conf)
{
    return ESP_OK;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
