#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef struct task_obj *TaskHandle_t;
typedef struct semaphore_obj *SemaphoreHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY ((TickType_t)0xffffffff)
