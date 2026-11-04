#pragma once
#include <stdint.h>
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int priority, void *handle, unsigned caps);
void vTaskDeleteWithCaps(void *handle);
