#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_flash_t esp_flash_t;
esp_err_t esp_flash_read_id(esp_flash_t *chip, uint32_t *out_id);
