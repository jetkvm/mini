#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"

BaseType_t xTaskCreateWithCaps(TaskFunction_t function, const char *name,
                               uint32_t stack_bytes, void *argument,
                               UBaseType_t priority, TaskHandle_t *task,
                               unsigned caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
