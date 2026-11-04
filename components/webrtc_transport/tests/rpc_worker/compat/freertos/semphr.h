#pragma once
/* A binary semaphore and the tick count for host tests (control_arena.c).
 * A take that finds no token first runs semaphore_wait_hook once, if a test
 * set it, as another task would run while the worker waits; without a token
 * then it returns at once with the wait added to the tick count. */
#include <stdint.h>
typedef uint32_t TickType_t;
typedef struct { int token; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buffer);
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
TickType_t xTaskGetTickCount(void);
extern void (*semaphore_wait_hook)(void);
