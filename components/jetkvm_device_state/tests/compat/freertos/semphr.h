#pragma once
#include <assert.h>
typedef struct { int held; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { s->held = 0; return s; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout) { (void)timeout; assert(!s->held); s->held = 1; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { assert(s->held); s->held = 0; return 1; }
