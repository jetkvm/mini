/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "jetkvm_mqtt.h"

esp_err_t jetkvm_mqtt_store_load(jetkvm_mqtt_config_t *config);
esp_err_t jetkvm_mqtt_store_save(const jetkvm_mqtt_config_t *config);

void jetkvm_mqtt_secure_zero(void *memory, size_t size);
