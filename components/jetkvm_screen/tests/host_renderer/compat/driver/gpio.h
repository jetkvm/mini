#pragma once
/* The two button pins: host_ui.c holds their levels. */
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
enum { GPIO_MODE_INPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_ANYEDGE };
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *arg);
int gpio_get_level(gpio_num_t pin);
