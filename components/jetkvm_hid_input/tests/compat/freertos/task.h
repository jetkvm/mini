#pragma once

#include "freertos/FreeRTOS.h"

BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                                  uint32_t stack_size, void *argument,
                                  UBaseType_t priority, TaskHandle_t *handle,
                                  BaseType_t core);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
