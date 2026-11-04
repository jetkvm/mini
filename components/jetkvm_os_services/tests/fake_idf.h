/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* Single-threaded stand-ins for the ESP-IDF and FreeRTOS calls of
 * jetkvm_os_services.c and, through fake_usb.h, os_services_usb.c.
 * run_host_tests.sh serves every ESP-IDF header those files include from
 * one of the two; the tests define the task calls. */

#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define esp_err_to_name(error) "error"

#define EXT_RAM_BSS_ATTR
#define CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES 524288
#define CONFIG_WEBRTC_TRANSPORT_AU_SLOTS 2

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
/* The memory policy admits every reservation. */
typedef struct {
    size_t psram_bytes, psram_largest_block;
} jetkvm_memory_policy_request_t;
typedef struct {
    int unused;
} jetkvm_memory_policy_reservation_t;
static inline bool jetkvm_memory_policy_reserve(
    const char *owner, const jetkvm_memory_policy_request_t *request,
    jetkvm_memory_policy_reservation_t *reservation)
{
    return true;
}
static inline bool jetkvm_memory_policy_reserve_psram(
    const char *owner, size_t bytes,
    jetkvm_memory_policy_reservation_t *reservation)
{
    return true;
}
static inline void jetkvm_memory_policy_release(
    jetkvm_memory_policy_reservation_t *reservation)
{
}
static inline void esp_fill_random(void *out, size_t length)
{
    memset(out, 0x5a, length);
}
static inline int64_t esp_timer_get_time(void)
{
    return 0;
}

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))

typedef struct fake_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry,
                                           const char *name, uint32_t stack,
                                           void *arg, UBaseType_t priority,
                                           TaskHandle_t *task, BaseType_t core,
                                           UBaseType_t caps);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);

/* One thread: every lock is free and nothing waits. */
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t fake_semaphore(void)
{
    static int semaphore;
    return &semaphore;
}
#define xSemaphoreCreateMutex fake_semaphore
#define xSemaphoreCreateRecursiveMutex fake_semaphore
#define xSemaphoreCreateBinary fake_semaphore
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    return pdTRUE;
}
#define xSemaphoreTakeRecursive xSemaphoreTake
#define xSemaphoreGiveRecursive xSemaphoreGive

/* A queue refuses an item when full, as FreeRTOS does after the wait. */
typedef struct {
    size_t capacity, item_size, count;
    unsigned char items[64];
} *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(UBaseType_t capacity,
                                         UBaseType_t item_size)
{
    QueueHandle_t queue = calloc(1U, sizeof(*queue));
    assert(capacity * item_size <= sizeof(queue->items));
    queue->capacity = capacity;
    queue->item_size = item_size;
    return queue;
}
static inline BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                                    TickType_t wait)
{
    if (queue->count == queue->capacity) {
        return pdFALSE;
    }
    memcpy(queue->items + queue->count++ * queue->item_size, item,
           queue->item_size);
    return pdTRUE;
}
static inline BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                                       TickType_t wait)
{
    if (queue->count == 0U) {
        return pdFALSE;
    }
    memcpy(item, queue->items, queue->item_size);
    memmove(queue->items, queue->items + queue->item_size,
            --queue->count * queue->item_size);
    return pdTRUE;
}

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
/* An empty namespace that takes every write. */
static inline esp_err_t nvs_open(const char *name, nvs_open_mode_t mode,
                                 nvs_handle_t *handle)
{
    return ESP_OK;
}
static inline esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                                     void *value, size_t *length)
{
    return ESP_FAIL;
}
static inline esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                                     const void *value, size_t length)
{
    return ESP_OK;
}
static inline esp_err_t nvs_commit(nvs_handle_t handle)
{
    return ESP_OK;
}
static inline void nvs_close(nvs_handle_t handle) {}
