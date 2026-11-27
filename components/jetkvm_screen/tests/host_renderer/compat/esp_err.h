#pragma once
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERROR_CHECK(expression) do { if ((expression) != ESP_OK) abort(); } while (0)
