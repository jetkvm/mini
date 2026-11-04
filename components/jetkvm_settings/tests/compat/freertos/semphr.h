#pragma once

#include "freertos/FreeRTOS.h"

typedef struct {
    int locked;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *storage)
{
    if (storage) {
        storage->locked = 0;
    }
    return storage;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                                        uint32_t timeout)
{
    (void)timeout;
    if (!semaphore || semaphore->locked) {
        return 0;
    }
    semaphore->locked = 1;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (!semaphore || !semaphore->locked) {
        return 0;
    }
    semaphore->locked = 0;
    return pdTRUE;
}
