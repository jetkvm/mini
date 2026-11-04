#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef struct fake_queue {
    pthread_mutex_t lock;
    size_t length;
    size_t item_size;
    size_t count;
    size_t head;
    uint8_t *storage;
} StaticQueue_t;

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *control);
BaseType_t xQueueReset(QueueHandle_t queue);
BaseType_t xQueueSendToFront(QueueHandle_t queue, const void *item,
                             TickType_t wait);
BaseType_t xQueueSendToBack(QueueHandle_t queue, const void *item,
                            TickType_t wait);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
