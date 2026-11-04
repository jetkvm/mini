#pragma once

#include "freertos/FreeRTOS.h"

typedef struct fake_semaphore {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    unsigned count;
    unsigned maximum;
} StaticSemaphore_t;

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
