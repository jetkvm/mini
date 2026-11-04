#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                               int priority, TaskHandle_t *task, uint32_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
