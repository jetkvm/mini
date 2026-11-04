#pragma once

#include <unistd.h>

#include "freertos/FreeRTOS.h"

/* The host test uses a 1 ms tick. */
static inline void vTaskDelay(TickType_t ticks)
{
    (void)usleep((useconds_t)ticks * 1000U);
}
