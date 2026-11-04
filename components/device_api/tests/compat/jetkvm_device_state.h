#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_cloud_config.h"

#define JETKVM_DEVICE_ID_CAPACITY 13U

typedef struct {
    bool registered;
    uint32_t generation;
    char cloud_url[JETKVM_CLOUD_URL_CAPACITY];
    char cloud_app_url[JETKVM_CLOUD_APP_URL_CAPACITY];
} jetkvm_device_cloud_public_state_t;

esp_err_t jetkvm_device_state_get_id(char *output, size_t output_capacity);
esp_err_t jetkvm_device_state_cloud_snapshot(
    jetkvm_cloud_config_t *config, uint32_t *generation);
esp_err_t jetkvm_device_state_cloud_public_snapshot(
    jetkvm_device_cloud_public_state_t *state);
esp_err_t jetkvm_device_state_cloud_set_registration_if_current(
    uint32_t expected_generation, const char *expected_cloud_url,
    const char *cloud_token, const char *google_identity);
