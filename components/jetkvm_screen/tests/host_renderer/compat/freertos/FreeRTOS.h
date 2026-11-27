#pragma once
/* The buttons code only notifies the screen task from its pin interrupt. */
#include <stdint.h>
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define pdFALSE 0
#define eSetBits 0
#define xTaskNotifyFromISR(task, bits, action, woken) ((void)(task), (void)(bits), (void)(woken))
#define portYIELD_FROM_ISR(woken) ((void)(woken))
