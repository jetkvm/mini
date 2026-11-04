#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t jetkvm_time_sync_start(void);
esp_err_t jetkvm_time_sync_wait(TickType_t timeout);
