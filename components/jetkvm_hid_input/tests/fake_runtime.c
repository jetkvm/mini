#define _POSIX_C_SOURCE 200809L

#include "fake_runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_control.h"

static void deadline_from_now(struct timespec *deadline, TickType_t wait)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += wait / 1000U;
    deadline->tv_nsec += (long)(wait % 1000U) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *control)
{
    memset(control, 0, sizeof(*control));
    pthread_mutex_init(&control->lock, NULL);
    control->length = length;
    control->item_size = item_size;
    control->storage = storage;
    return control;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    pthread_mutex_lock(&queue->lock);
    queue->count = 0U;
    queue->head = 0U;
    pthread_mutex_unlock(&queue->lock);
    return pdPASS;
}

static atomic_bool s_fail_next_send;
void fake_queue_fail_next_send(void) { atomic_store(&s_fail_next_send, true); }
static BaseType_t queue_send(QueueHandle_t queue, const void *item, bool front)
{
    if (atomic_exchange(&s_fail_next_send, false)) return pdFALSE;
    pthread_mutex_lock(&queue->lock);
    if (queue->count == queue->length) {
        pthread_mutex_unlock(&queue->lock);
        return pdFALSE;
    }
    size_t index;
    if (front) {
        queue->head = (queue->head + queue->length - 1U) % queue->length;
        index = queue->head;
    } else {
        index = (queue->head + queue->count) % queue->length;
    }
    memcpy(queue->storage + index * queue->item_size, item, queue->item_size);
    queue->count++;
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

BaseType_t xQueueSendToFront(QueueHandle_t queue, const void *item,
                             TickType_t wait)
{
    (void)wait;
    return queue_send(queue, item, true);
}

BaseType_t xQueueSendToBack(QueueHandle_t queue, const void *item,
                            TickType_t wait)
{
    (void)wait;
    return queue_send(queue, item, false);
}

BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    pthread_mutex_lock(&queue->lock);
    if (queue->length != 1U) {
        pthread_mutex_unlock(&queue->lock);
        return pdFALSE;
    }
    memcpy(queue->storage, item, queue->item_size);
    queue->head = 0U;
    queue->count = 1U;
    pthread_mutex_unlock(&queue->lock);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    (void)wait;
    pthread_mutex_lock(&queue->lock);
    if (queue->count == 0U) {
        pthread_mutex_unlock(&queue->lock);
        return pdFALSE;
    }
    memcpy(item, queue->storage + queue->head * queue->item_size,
           queue->item_size);
    queue->head = (queue->head + 1U) % queue->length;
    queue->count--;
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    pthread_mutex_lock(&queue->lock);
    UBaseType_t count = (UBaseType_t)queue->count;
    pthread_mutex_unlock(&queue->lock);
    return count;
}

static SemaphoreHandle_t semaphore_create(StaticSemaphore_t *storage,
                                           unsigned initial)
{
    memset(storage, 0, sizeof(*storage));
    pthread_mutex_init(&storage->lock, NULL);
    pthread_cond_init(&storage->condition, NULL);
    storage->count = initial;
    storage->maximum = 1U;
    return storage;
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    return semaphore_create(storage, 0U);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return semaphore_create(storage, 1U);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait)
{
    pthread_mutex_lock(&semaphore->lock);
    if (wait == 0U && semaphore->count == 0U) {
        pthread_mutex_unlock(&semaphore->lock);
        return pdFALSE;
    }
    while (semaphore->count == 0U) {
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&semaphore->condition, &semaphore->lock);
            continue;
        }
        struct timespec deadline;
        deadline_from_now(&deadline, wait);
        if (pthread_cond_timedwait(&semaphore->condition, &semaphore->lock,
                                   &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&semaphore->lock);
            return pdFALSE;
        }
    }
    semaphore->count--;
    pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    pthread_mutex_lock(&semaphore->lock);
    semaphore->count = semaphore->maximum;
    pthread_cond_signal(&semaphore->condition);
    pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

struct fake_task {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    uint32_t notifications;
    void (*entry)(void *);
    void *argument;
};

static _Thread_local TaskHandle_t s_current_task;

static void *task_wrapper(void *argument)
{
    TaskHandle_t task = argument;
    s_current_task = task;
    task->entry(task->argument);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                                  uint32_t stack_size, void *argument,
                                  UBaseType_t priority, TaskHandle_t *handle,
                                  BaseType_t core)
{
    (void)name;
    (void)stack_size;
    (void)priority;
    (void)core;
    TaskHandle_t task = calloc(1U, sizeof(*task));
    if (!task) {
        return pdFAIL;
    }
    pthread_mutex_init(&task->lock, NULL);
    pthread_cond_init(&task->condition, NULL);
    task->entry = entry;
    task->argument = argument;
    if (pthread_create(&task->thread, NULL, task_wrapper, task) != 0) {
        free(task);
        return pdFAIL;
    }
    *handle = task;
    return pdPASS;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    pthread_mutex_lock(&task->lock);
    task->notifications++;
    pthread_cond_signal(&task->condition);
    pthread_mutex_unlock(&task->lock);
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait)
{
    TaskHandle_t task = s_current_task;
    pthread_mutex_lock(&task->lock);
    while (task->notifications == 0U) {
        if (wait == 0U) {
            pthread_mutex_unlock(&task->lock);
            return 0U;
        }
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&task->condition, &task->lock);
            continue;
        }
        struct timespec deadline;
        deadline_from_now(&deadline, wait);
        if (pthread_cond_timedwait(&task->condition, &task->lock,
                                   &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&task->lock);
            return 0U;
        }
    }
    uint32_t previous = task->notifications;
    if (clear_on_exit) {
        task->notifications = 0U;
    } else {
        task->notifications--;
    }
    pthread_mutex_unlock(&task->lock);
    return previous;
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec delay = {
        .tv_sec = ticks / 1000U,
        .tv_nsec = (long)(ticks % 1000U) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
    pthread_exit(NULL);
}

#define MAX_REPORTS 256U
static pthread_mutex_t s_usb_lock = PTHREAD_MUTEX_INITIALIZER;
static jetkvm_usb_hid_keyboard_state_t s_keyboard;
static jetkvm_usb_hid_state_t s_usb_state = {
    .initialized = true,
    .connected = true,
    .mounted = true,
    .link_high_speed = true,
};
static esp_err_t s_wake_result = ESP_OK;
static unsigned s_wake_call_count;
static fake_keyboard_report_t s_reports[MAX_REPORTS];
static size_t s_report_count;
static unsigned s_release_all_count;
static jetkvm_usb_hid_led_callback_t s_led_callback;
static void *s_led_context;

static void record_keyboard(uint8_t modifier, const uint8_t *keys)
{
    pthread_mutex_lock(&s_usb_lock);
    s_keyboard.modifier = modifier;
    if (keys) {
        memcpy(s_keyboard.keycodes, keys, sizeof(s_keyboard.keycodes));
    } else {
        memset(s_keyboard.keycodes, 0, sizeof(s_keyboard.keycodes));
    }
    if (s_report_count < MAX_REPORTS) {
        fake_keyboard_report_t *report = &s_reports[s_report_count++];
        report->timestamp_us = esp_timer_get_time();
        report->modifier = modifier;
        memcpy(report->keys, s_keyboard.keycodes, sizeof(report->keys));
    }
    pthread_mutex_unlock(&s_usb_lock);
}

void fake_usb_reset_log(void)
{
    pthread_mutex_lock(&s_usb_lock);
    s_report_count = 0U;
    s_release_all_count = 0U;
    pthread_mutex_unlock(&s_usb_lock);
}

size_t fake_usb_report_count(void)
{
    pthread_mutex_lock(&s_usb_lock);
    size_t count = s_report_count;
    pthread_mutex_unlock(&s_usb_lock);
    return count;
}

fake_keyboard_report_t fake_usb_report_at(size_t index)
{
    pthread_mutex_lock(&s_usb_lock);
    fake_keyboard_report_t report = s_reports[index];
    pthread_mutex_unlock(&s_usb_lock);
    return report;
}

unsigned fake_usb_release_all_count(void)
{
    pthread_mutex_lock(&s_usb_lock);
    unsigned count = s_release_all_count;
    pthread_mutex_unlock(&s_usb_lock);
    return count;
}

void fake_usb_set_state(jetkvm_usb_hid_state_t state)
{
    pthread_mutex_lock(&s_usb_lock);
    s_usb_state = state;
    pthread_mutex_unlock(&s_usb_lock);
}

void fake_usb_set_wake_result(esp_err_t result)
{
    pthread_mutex_lock(&s_usb_lock);
    s_wake_result = result;
    pthread_mutex_unlock(&s_usb_lock);
}

unsigned fake_usb_wake_call_count(void)
{
    pthread_mutex_lock(&s_usb_lock);
    unsigned count = s_wake_call_count;
    pthread_mutex_unlock(&s_usb_lock);
    return count;
}

void fake_usb_emit_led(uint8_t leds)
{
    pthread_mutex_lock(&s_usb_lock);
    s_usb_state.keyboard_leds = leds;
    jetkvm_usb_hid_led_callback_t callback = s_led_callback;
    void *context = s_led_context;
    pthread_mutex_unlock(&s_usb_lock);
    if (callback != NULL) {
        callback(leds, context);
    }
}

esp_err_t jetkvm_usb_hid_get_state(jetkvm_usb_hid_state_t *state)
{
    pthread_mutex_lock(&s_usb_lock);
    *state = s_usb_state;
    pthread_mutex_unlock(&s_usb_lock);
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_get_configuration_snapshot(
    jetkvm_usb_hid_configuration_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_usb_lock);
    *snapshot = (jetkvm_usb_hid_configuration_snapshot_t) {
        .vendor_id = 0x303a,
        .product_id = 0x400c,
        .serial_number = "ESP32-P4",
        .manufacturer = "JetKVM",
        .product = "JetKVM USB Emulation Device",
        .absolute_mouse = true,
        .relative_mouse = true,
        .keyboard = true,
        .emulation_enabled = s_usb_state.initialized,
    };
    pthread_mutex_unlock(&s_usb_lock);
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_remote_wakeup(void)
{
    pthread_mutex_lock(&s_usb_lock);
    s_wake_call_count++;
    esp_err_t result = s_wake_result;
    pthread_mutex_unlock(&s_usb_lock);
    return result;
}

esp_err_t jetkvm_usb_hid_get_keyboard_state(
    jetkvm_usb_hid_keyboard_state_t *state)
{
    pthread_mutex_lock(&s_usb_lock);
    *state = s_keyboard;
    pthread_mutex_unlock(&s_usb_lock);
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_keyboard_report(uint8_t modifier,
                                         const uint8_t keycodes[
                                             JETKVM_USB_HID_KEY_COUNT])
{
    pthread_mutex_lock(&s_usb_lock);
    bool reports_allowed = s_usb_state.initialized && s_usb_state.mounted &&
                           !s_usb_state.suspended;
    pthread_mutex_unlock(&s_usb_lock);
    if (!reports_allowed) {
        return ESP_ERR_INVALID_STATE;
    }
    record_keyboard(modifier, keycodes);
    return ESP_OK;
}

static esp_err_t s_keypress_result = ESP_OK;
static bool s_bus_events_waiting;

void fake_usb_set_keypress_result(esp_err_t result)
{
    pthread_mutex_lock(&s_usb_lock);
    s_keypress_result = result;
    pthread_mutex_unlock(&s_usb_lock);
}

void fake_usb_set_bus_events_waiting(bool waiting)
{
    pthread_mutex_lock(&s_usb_lock);
    s_bus_events_waiting = waiting;
    pthread_mutex_unlock(&s_usb_lock);
}

bool jetkvm_usb_hid_bus_events_waiting(void)
{
    pthread_mutex_lock(&s_usb_lock);
    bool waiting = s_bus_events_waiting;
    pthread_mutex_unlock(&s_usb_lock);
    return waiting;
}

esp_err_t jetkvm_usb_hid_keypress(uint8_t usage, bool pressed)
{
    pthread_mutex_lock(&s_usb_lock);
    jetkvm_usb_hid_keyboard_state_t next = s_keyboard;
    esp_err_t result = s_keypress_result;
    pthread_mutex_unlock(&s_usb_lock);
    if (result != ESP_OK) {
        return result;
    }
    if (usage >= 0xe0U && usage <= 0xe7U) {
        uint8_t mask = (uint8_t)(1U << (usage - 0xe0U));
        next.modifier = pressed ? (uint8_t)(next.modifier | mask) :
                                  (uint8_t)(next.modifier & (uint8_t)~mask);
    } else if (pressed) {
        for (size_t i = 0; i < JETKVM_USB_HID_KEY_COUNT; ++i) {
            if (next.keycodes[i] == usage) {
                break;
            }
            if (next.keycodes[i] == 0U) {
                next.keycodes[i] = usage;
                break;
            }
        }
    } else {
        for (size_t i = 0; i < JETKVM_USB_HID_KEY_COUNT; ++i) {
            if (next.keycodes[i] == usage) {
                next.keycodes[i] = 0U;
            }
        }
    }
    record_keyboard(next.modifier, next.keycodes);
    return ESP_OK;
}

static fake_mouse_report_t s_mouse_reports[32];
static size_t s_mouse_count;
size_t fake_usb_mouse_count(void)
{
    pthread_mutex_lock(&s_usb_lock);
    size_t count = s_mouse_count;
    pthread_mutex_unlock(&s_usb_lock);
    return count;
}
fake_mouse_report_t fake_usb_mouse_at(size_t index)
{
    pthread_mutex_lock(&s_usb_lock);
    fake_mouse_report_t report = s_mouse_reports[index];
    pthread_mutex_unlock(&s_usb_lock);
    return report;
}
static void record_mouse(bool relative, int32_t x, int32_t y, uint8_t buttons)
{
    pthread_mutex_lock(&s_usb_lock);
    if (s_mouse_count < 32) s_mouse_reports[s_mouse_count++] =
        (fake_mouse_report_t){relative, x, y, buttons};
    pthread_mutex_unlock(&s_usb_lock);
}

esp_err_t jetkvm_usb_hid_absolute_mouse_report(int32_t x, int32_t y,
                                                uint8_t buttons)
{
    record_mouse(false, x, y, buttons);
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_absolute_wheel_report(int8_t wheel_y,
                                                int8_t wheel_x)
{
    (void)wheel_y;
    (void)wheel_x;
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_wheel_report(int8_t wheel_y, int8_t wheel_x)
{
    return jetkvm_usb_hid_absolute_wheel_report(wheel_y, wheel_x);
}

esp_err_t jetkvm_usb_hid_relative_mouse_report(int8_t dx, int8_t dy,
                                                uint8_t buttons,
                                                int8_t wheel_y,
                                                int8_t wheel_x)
{
    record_mouse(true, dx, dy, buttons);
    (void)wheel_y;
    (void)wheel_x;
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_release_all(void)
{
    pthread_mutex_lock(&s_usb_lock);
    s_release_all_count++;
    bool reports_allowed = s_usb_state.initialized && s_usb_state.mounted &&
                           !s_usb_state.suspended;
    pthread_mutex_unlock(&s_usb_lock);
    if (!reports_allowed) {
        return ESP_ERR_INVALID_STATE;
    }
    record_keyboard(0U, NULL);
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_set_led_callback(
    jetkvm_usb_hid_led_callback_t callback, void *ctx)
{
    s_led_callback = callback;
    s_led_context = ctx;
    (void)s_led_callback;
    (void)s_led_context;
    return ESP_OK;
}

bool jetkvm_hidrpc_macro_step_at(
    const jetkvm_hidrpc_keyboard_macro_report_t *macro, uint32_t index,
    jetkvm_hidrpc_macro_step_t *step)
{
    if (!macro || !step || macro->step_count > JETKVM_HIDRPC_MACRO_MAX_STEPS ||
        index >= macro->step_count || !macro->encoded_steps ||
        macro->encoded_steps_size !=
            (size_t)macro->step_count * JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE) {
        return false;
    }
    const uint8_t *encoded = macro->encoded_steps +
        (size_t)index * JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE;
    step->modifier = encoded[0];
    memcpy(step->keys, encoded + 1U, JETKVM_HIDRPC_KEY_COUNT);
    step->delay_ms = (uint16_t)((uint16_t)encoded[7] << 8) | encoded[8];
    return true;
}
