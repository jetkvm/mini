#pragma once

#include "task.h"

static inline BaseType_t xTaskCreateWithCaps(
    TaskFunction_t function, const char *name, uint32_t stack_size,
    void *argument, unsigned priority, TaskHandle_t *task,
    unsigned memory_caps)
{
    (void)memory_caps;
    return xTaskCreate(function, name, stack_size, argument, priority, task);
}

static inline void vTaskDeleteWithCaps(TaskHandle_t task)
{
    vTaskDelete(task);
}
