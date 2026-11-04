/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_device_state.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "jetkvm_memory_policy.h"

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    bool initialized;
    bool deregistration_pending;
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    jetkvm_cloud_config_t cloud;
    uint32_t cloud_generation;
    esp_err_t persistence_error;
    jetkvm_device_cloud_change_cb_t observer;
    void *observer_ctx;
} device_state_t;

static device_state_t s_state;

static void secure_zero(void *value, size_t size)
{
    volatile unsigned char *bytes = value;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static jetkvm_cloud_config_t *cloud_config_alloc(void)
{
    jetkvm_cloud_config_t *config =
        jetkvm_memory_policy_control_alloc(sizeof(*config));
    if (config) {
        memset(config, 0, sizeof(*config));
    }
    return config;
}

static void cloud_config_free(jetkvm_cloud_config_t *config)
{
    if (config) {
        secure_zero(config, sizeof(*config));
        jetkvm_memory_policy_control_free(config);
    }
}

static uint32_t next_nonzero(uint32_t generation)
{
    ++generation;
    return generation ? generation : 1U;
}

static uint32_t cloud_changes(const jetkvm_cloud_config_t *before,
                              const jetkvm_cloud_config_t *after)
{
    uint32_t changes = 0U;
    if (strcmp(before->cloud_url, after->cloud_url) != 0) {
        changes |= JETKVM_DEVICE_CLOUD_CHANGE_API_URL;
    }
    if (strcmp(before->cloud_app_url, after->cloud_app_url) != 0) {
        changes |= JETKVM_DEVICE_CLOUD_CHANGE_APP_URL;
    }
    if (strcmp(before->cloud_token, after->cloud_token) != 0 ||
        strcmp(before->google_identity, after->google_identity) != 0) {
        changes |= JETKVM_DEVICE_CLOUD_CHANGE_REGISTRATION;
    }
    return changes;
}

static void publish_cloud_locked(
    const jetkvm_cloud_config_t *config, uint32_t changes,
    jetkvm_device_cloud_change_cb_t *observer, void **observer_ctx,
    uint32_t *generation)
{
    s_state.cloud = *config;
    s_state.persistence_error = ESP_OK;
    if (changes) {
        s_state.cloud_generation = next_nonzero(s_state.cloud_generation);
        *observer = s_state.observer;
        *observer_ctx = s_state.observer_ctx;
    }
    *generation = s_state.cloud_generation;
}

static void notify_cloud_change(jetkvm_device_cloud_change_cb_t observer,
                                void *observer_ctx, uint32_t changes,
                                uint32_t generation)
{
    if (changes && observer) {
        observer(changes, generation, observer_ctx);
    }
}

static esp_err_t require_initialized(void)
{
    return s_state.initialized && s_state.lock ? ESP_OK :
                                                 ESP_ERR_INVALID_STATE;
}

esp_err_t jetkvm_device_state_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }
    memset(&s_state, 0, sizeof(s_state));
    s_state.lock = xSemaphoreCreateMutexStatic(&s_state.lock_storage);
    if (!s_state.lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t identity_error = jetkvm_device_id_get(
        s_state.device_id, sizeof(s_state.device_id));
    if (identity_error != ESP_OK) {
        memset(&s_state, 0, sizeof(s_state));
        return identity_error;
    }

    esp_err_t load_error = jetkvm_cloud_config_load(&s_state.cloud);
    if (load_error != ESP_OK) {
        /* Corrupt/unknown cloud state must not prevent local video startup. */
        jetkvm_cloud_config_defaults(&s_state.cloud);
        s_state.persistence_error = load_error;
    }
    s_state.cloud_generation = 1U;
    s_state.initialized = true;
    return load_error;
}

esp_err_t jetkvm_device_state_get_id(char *output, size_t output_capacity)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!output || output_capacity < JETKVM_DEVICE_ID_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    memcpy(output, s_state.device_id, sizeof(s_state.device_id));
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_snapshot(
    jetkvm_cloud_config_t *config, uint32_t *generation)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    *config = s_state.cloud;
    if (generation) {
        *generation = s_state.cloud_generation;
    }
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_public_snapshot(
    jetkvm_device_cloud_public_state_t *state)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    memset(state, 0, sizeof(*state));
    state->registered = jetkvm_cloud_config_is_registered(&s_state.cloud);
    state->generation = s_state.cloud_generation;
    memcpy(state->cloud_url, s_state.cloud.cloud_url,
           sizeof(state->cloud_url));
    memcpy(state->cloud_app_url, s_state.cloud.cloud_app_url,
           sizeof(state->cloud_app_url));
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_set_urls(
    const char *cloud_url, const char *cloud_app_url,
    uint32_t *changes_out)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    jetkvm_device_cloud_change_cb_t observer = NULL;
    void *observer_ctx = NULL;
    uint32_t generation = 0U;
    uint32_t changes = 0U;
    jetkvm_cloud_config_t *staged = cloud_config_alloc();
    if (!staged) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.deregistration_pending) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return ESP_ERR_INVALID_STATE;
    }
    *staged = s_state.cloud;
    esp_err_t err = jetkvm_cloud_config_set_urls(
        staged, cloud_url, cloud_app_url);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return err;
    }
    changes = cloud_changes(&s_state.cloud, staged);
    if (changes) {
        err = jetkvm_cloud_config_save(staged);
        if (err != ESP_OK) {
            s_state.persistence_error = err;
            xSemaphoreGive(s_state.lock);
            cloud_config_free(staged);
            return err;
        }
        publish_cloud_locked(staged, changes, &observer, &observer_ctx,
                             &generation);
    }
    xSemaphoreGive(s_state.lock);
    cloud_config_free(staged);
    if (changes_out) {
        *changes_out = changes;
    }
    notify_cloud_change(observer, observer_ctx, changes, generation);
    return ESP_OK;
}

static esp_err_t set_registration_transaction(
    bool require_current, uint32_t expected_generation,
    const char *expected_cloud_url, const char *cloud_token,
    const char *google_identity)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    jetkvm_device_cloud_change_cb_t observer = NULL;
    void *observer_ctx = NULL;
    uint32_t generation = 0U;
    uint32_t changes = 0U;
    jetkvm_cloud_config_t *staged = cloud_config_alloc();
    if (!staged) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.deregistration_pending) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return ESP_ERR_INVALID_STATE;
    }
    if (require_current &&
        (expected_generation == 0U || !expected_cloud_url ||
         s_state.cloud_generation != expected_generation ||
         strcmp(s_state.cloud.cloud_url, expected_cloud_url) != 0)) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return ESP_ERR_INVALID_STATE;
    }
    *staged = s_state.cloud;
    esp_err_t err = jetkvm_cloud_config_set_registration(
        staged, cloud_token, google_identity);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return err;
    }
    err = jetkvm_cloud_config_save(staged);
    if (err != ESP_OK) {
        s_state.persistence_error = err;
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return err;
    }
    changes = cloud_changes(&s_state.cloud, staged);
    publish_cloud_locked(staged, changes, &observer, &observer_ctx,
                         &generation);
    xSemaphoreGive(s_state.lock);
    cloud_config_free(staged);
    notify_cloud_change(observer, observer_ctx, changes, generation);
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_set_registration(
    const char *cloud_token, const char *google_identity)
{
    return set_registration_transaction(false, 0U, NULL, cloud_token,
                                        google_identity);
}

esp_err_t jetkvm_device_state_cloud_set_registration_if_current(
    uint32_t expected_generation, const char *expected_cloud_url,
    const char *cloud_token, const char *google_identity)
{
    return set_registration_transaction(true, expected_generation,
                                        expected_cloud_url, cloud_token,
                                        google_identity);
}

static esp_err_t clear_registration_transaction(
    bool require_current, uint32_t expected_generation,
    const char *expected_cloud_url, bool defer_publish)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    jetkvm_device_cloud_change_cb_t observer = NULL;
    void *observer_ctx = NULL;
    uint32_t generation = 0U;
    uint32_t changes = 0U;
    jetkvm_cloud_config_t *staged = cloud_config_alloc();
    if (!staged) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.deregistration_pending) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return ESP_ERR_INVALID_STATE;
    }
    if (require_current &&
        (expected_generation == 0U || !expected_cloud_url ||
         s_state.cloud_generation != expected_generation ||
         strcmp(s_state.cloud.cloud_url, expected_cloud_url) != 0)) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return ESP_ERR_INVALID_STATE;
    }
    *staged = s_state.cloud;
    jetkvm_cloud_config_clear_registration(staged);
    changes = cloud_changes(&s_state.cloud, staged);
    if (!changes) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return ESP_OK;
    }
    esp_err_t err = jetkvm_cloud_config_save(staged);
    if (err != ESP_OK) {
        s_state.persistence_error = err;
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return err;
    }
    if (defer_publish) {
        s_state.deregistration_pending = true;
        s_state.persistence_error = ESP_OK;
        xSemaphoreGive(s_state.lock);
        cloud_config_free(staged);
        return ESP_OK;
    }
    publish_cloud_locked(staged, changes, &observer, &observer_ctx,
                         &generation);
    xSemaphoreGive(s_state.lock);
    cloud_config_free(staged);
    notify_cloud_change(observer, observer_ctx, changes, generation);
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_reload(void)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    jetkvm_device_cloud_change_cb_t observer = NULL;
    void *observer_ctx = NULL;
    uint32_t generation = 0U;
    jetkvm_cloud_config_t *loaded = cloud_config_alloc();
    if (!loaded) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.deregistration_pending) {
        xSemaphoreGive(s_state.lock);
        cloud_config_free(loaded);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = jetkvm_cloud_config_load(loaded);
    if (err != ESP_OK) {
        s_state.persistence_error = err;
        xSemaphoreGive(s_state.lock);
        cloud_config_free(loaded);
        return err;
    }
    uint32_t changes = cloud_changes(&s_state.cloud, loaded);
    publish_cloud_locked(loaded, changes, &observer, &observer_ctx,
                         &generation);
    xSemaphoreGive(s_state.lock);
    cloud_config_free(loaded);
    notify_cloud_change(observer, observer_ctx, changes, generation);
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_subscribe(
    jetkvm_device_cloud_change_cb_t callback, void *ctx)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.observer &&
        (s_state.observer != callback || s_state.observer_ctx != ctx)) {
        xSemaphoreGive(s_state.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state.observer = callback;
    s_state.observer_ctx = ctx;
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

void jetkvm_device_state_cloud_unsubscribe(
    jetkvm_device_cloud_change_cb_t callback, void *ctx)
{
    if (require_initialized() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.observer == callback && s_state.observer_ctx == ctx) {
        s_state.observer = NULL;
        s_state.observer_ctx = NULL;
    }
    xSemaphoreGive(s_state.lock);
}

esp_err_t jetkvm_device_state_persistence_error(void)
{
    if (require_initialized() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    esp_err_t err = s_state.persistence_error;
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t jetkvm_device_state_cloud_clear_registration(void)
{
    return clear_registration_transaction(false, 0U, NULL, false);
}

esp_err_t jetkvm_device_state_cloud_clear_registration_if_current(
    uint32_t expected_generation, const char *expected_cloud_url)
{
    return clear_registration_transaction(true, expected_generation,
                                          expected_cloud_url, false);
}

esp_err_t jetkvm_device_state_cloud_prepare_deregistration(
    uint32_t expected_generation, const char *expected_cloud_url)
{
    return clear_registration_transaction(true, expected_generation,
                                          expected_cloud_url, true);
}

void jetkvm_device_state_cloud_finish_deregistration(void)
{
    if (require_initialized() != ESP_OK) return;
    jetkvm_device_cloud_change_cb_t observer = NULL;
    void *observer_ctx = NULL;
    uint32_t generation = 0;
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.deregistration_pending) {
        jetkvm_cloud_config_clear_registration(&s_state.cloud);
        s_state.deregistration_pending = false;
        s_state.cloud_generation = next_nonzero(s_state.cloud_generation);
        generation = s_state.cloud_generation;
        observer = s_state.observer;
        observer_ctx = s_state.observer_ctx;
    }
    xSemaphoreGive(s_state.lock);
    notify_cloud_change(observer, observer_ctx,
        JETKVM_DEVICE_CLOUD_CHANGE_REGISTRATION, generation);
}
