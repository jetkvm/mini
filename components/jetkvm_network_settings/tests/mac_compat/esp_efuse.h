#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef struct esp_efuse_desc esp_efuse_desc_t;
int esp_efuse_get_field_size(const esp_efuse_desc_t *field[]);
esp_err_t esp_efuse_read_field_blob(const esp_efuse_desc_t *field[], void *dst, size_t dst_size_bits);
