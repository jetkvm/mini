/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "jetkvm_control.h"
#include "jetkvm_ota.h"

_Static_assert(JETKVM_CONTROL_OTA_VERSION_CAPACITY ==
                   JETKVM_OTA_VERSION_CAPACITY,
               "OTA version capacities must match");
_Static_assert(JETKVM_CONTROL_OTA_URL_CAPACITY == JETKVM_OTA_URL_CAPACITY,
               "OTA URL capacities must match");
_Static_assert(JETKVM_CONTROL_OTA_HASH_CAPACITY == JETKVM_OTA_HASH_CAPACITY,
               "OTA hash capacities must match");
_Static_assert(JETKVM_CONTROL_OTA_ERROR_CAPACITY == JETKVM_OTA_ERROR_CAPACITY,
               "OTA error capacities must match");
_Static_assert(JETKVM_CONTROL_OTA_API_URL_CAPACITY ==
                   JETKVM_OTA_API_URL_CAPACITY,
               "OTA API URL capacities must match");

static void ota_native_to_control(const jetkvm_ota_update_status_t *native,
                                  jetkvm_control_ota_status_t *control)
{
    memset(control, 0, sizeof(*control));
    memcpy(control->local_app_version, native->local_app_version,
           sizeof(control->local_app_version));
    memcpy(control->local_system_version, native->local_system_version,
           sizeof(control->local_system_version));
    memcpy(control->remote_app_version, native->remote.app_version,
           sizeof(control->remote_app_version));
    memcpy(control->remote_app_url, native->remote.app_url,
           sizeof(control->remote_app_url));
    memcpy(control->remote_app_hash, native->remote.app_hash,
           sizeof(control->remote_app_hash));
    memcpy(control->remote_system_version, native->remote.system_version,
           sizeof(control->remote_system_version));
    memcpy(control->remote_system_url, native->remote.system_url,
           sizeof(control->remote_system_url));
    memcpy(control->remote_system_hash, native->remote.system_hash,
           sizeof(control->remote_system_hash));
    control->app_update_available = native->app_update_available;
    control->system_update_available = native->system_update_available;
    control->will_disable_auto_update = native->will_disable_auto_update;
    memcpy(control->error, native->error, sizeof(control->error));
}

static bool ota_rpc_get_update_status(jetkvm_control_ota_status_t *status,
                                      void *ctx)
{
    (void)ctx;
    /* Go's getUpdateStatus performs the metadata fetch; the frontend relies
     * on it for every "Check for Updates" interaction. */
    jetkvm_ota_config_t config;
    jetkvm_ota_update_status_t native;
    if (jetkvm_ota_get_config(&config) != ESP_OK ||
        jetkvm_ota_check_update(NULL, NULL, config.include_prerelease,
                                &native) != ESP_OK) {
        memset(&config, 0, sizeof(config));
        return false;
    }
    memset(&config, 0, sizeof(config));
    ota_native_to_control(&native, status);
    memset(&native, 0, sizeof(native));
    return true;
}

static bool ota_rpc_check_update(const char *app_constraint,
                                 const char *system_constraint,
                                 bool include_prerelease,
                                 jetkvm_control_ota_status_t *status,
                                 void *ctx)
{
    (void)ctx;
    jetkvm_ota_update_status_t native;
    if (jetkvm_ota_check_update(app_constraint, system_constraint,
                                include_prerelease, &native) != ESP_OK) {
        return false;
    }
    ota_native_to_control(&native, status);
    memset(&native, 0, sizeof(native));
    return true;
}

static bool ota_rpc_try_update(const char *app_constraint,
                               const char *system_constraint,
                               bool include_prerelease, bool reset_config,
                               char *error, size_t error_capacity, void *ctx)
{
    (void)ctx;
    esp_err_t result = jetkvm_ota_try_update(
        app_constraint, system_constraint, include_prerelease, reset_config);
    if (result != ESP_OK && error != NULL && error_capacity > 0U) {
        (void)snprintf(error, error_capacity, "%s",
                       "an update is already in progress");
    }
    return result == ESP_OK;
}

static bool ota_rpc_get_auto_update(void *ctx)
{
    (void)ctx;
    jetkvm_ota_config_t config;
    if (jetkvm_ota_get_config(&config) != ESP_OK) {
        return false;
    }
    return config.auto_update_enabled;
}

static bool ota_rpc_set_auto_update(bool enabled, void *ctx)
{
    (void)ctx;
    if (jetkvm_ota_set_auto_update(enabled) != ESP_OK) {
        return false;
    }
    return enabled;
}

static bool ota_rpc_get_dev_channel(void *ctx)
{
    (void)ctx;
    jetkvm_ota_config_t config;
    if (jetkvm_ota_get_config(&config) != ESP_OK) {
        return false;
    }
    return config.include_prerelease;
}

static bool ota_rpc_set_dev_channel(bool enabled, void *ctx)
{
    (void)ctx;
    if (jetkvm_ota_set_include_prerelease(enabled) != ESP_OK) {
        return false;
    }
    return enabled;
}

static bool ota_rpc_is_update_pending(void *ctx)
{
    (void)ctx;
    return jetkvm_ota_is_update_pending();
}

static bool ota_rpc_get_update_api_url(char *url, size_t capacity, void *ctx)
{
    (void)ctx;
    jetkvm_ota_config_t config;
    if (jetkvm_ota_get_config(&config) != ESP_OK ||
        strlen(config.update_api_url) >= capacity) {
        return false;
    }
    memcpy(url, config.update_api_url, strlen(config.update_api_url) + 1U);
    memset(&config, 0, sizeof(config));
    return true;
}

static bool ota_rpc_set_update_api_url(const char *url, char *error,
                                       size_t error_capacity, void *ctx)
{
    (void)ctx;
    esp_err_t result = jetkvm_ota_set_update_api_url(url);
    if (result != ESP_OK && error != NULL && error_capacity > 0U) {
        (void)snprintf(error, error_capacity, "%s",
                       result == ESP_ERR_INVALID_ARG ?
                           "invalid update server URL" :
                           "failed to persist the update server URL");
    }
    return result == ESP_OK;
}

void jetkvm_ota_fill_control_context(jetkvm_ota_context_t *context)
{
    *context = (jetkvm_ota_context_t){
        .get_update_status = ota_rpc_get_update_status,
        .check_update = ota_rpc_check_update,
        .try_update = ota_rpc_try_update,
        .get_auto_update = ota_rpc_get_auto_update,
        .set_auto_update = ota_rpc_set_auto_update,
        .get_dev_channel = ota_rpc_get_dev_channel,
        .set_dev_channel = ota_rpc_set_dev_channel,
        .is_update_pending = ota_rpc_is_update_pending,
        .get_update_api_url = ota_rpc_get_update_api_url,
        .set_update_api_url = ota_rpc_set_update_api_url,
    };
}
