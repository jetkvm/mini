/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "jetkvm_hid_profiles.h"

#include <inttypes.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jetkvm_hid_input.h"

#define JIGGLER_TASK_NAME "jetkvm_jiggler"
#define JIGGLER_TASK_STACK_SIZE 4096U
#define JIGGLER_TASK_PRIORITY 2U
#define JIGGLER_POLL_MS 100U

static const char *TAG = "jetkvm_jiggler";
static portMUX_TYPE s_jiggler_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_jiggler_task;
static bool s_jiggler_starting;

static int8_t random_delta(void)
{
    int8_t value = (int8_t)(esp_random() % 3U + 1U);
    return (esp_random() & 1U) != 0U ? value : (int8_t)-value;
}

static void jiggler_task(void *ctx)
{
    (void)ctx;
    uint32_t observed_revision = 0U;
    time_t last_checked_second = (time_t)-1;
    time_t previous_match = 0;
    bool pending = false;
    int64_t pending_at_us = 0;
    uint32_t pending_revision = 0U;

    for (;;) {
        bool enabled = false;
        uint32_t revision = 0U;
        jetkvm_control_jiggler_config_t config = {0};
        if (jetkvm_hid_profiles_get_jiggler_snapshot(
                &enabled, &config, &revision) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(JIGGLER_POLL_MS));
            continue;
        }
        if (revision != observed_revision) {
            observed_revision = revision;
            last_checked_second = (time_t)-1;
            previous_match = 0;
            pending = false;
        }

        time_t now = time(NULL);
        if (enabled && now != last_checked_second) {
            last_checked_second = now;
            struct tm utc = {0};
            if (gmtime_r(&now, &utc) != NULL &&
                jetkvm_hid_profiles_cron_matches(
                    config.schedule_cron_tab, &utc)) {
                uint64_t interval_us = previous_match > 0 &&
                                               now > previous_match
                                           ? (uint64_t)(now - previous_match) *
                                                 UINT64_C(1000000)
                                           : 0U;
                previous_match = now;
                uint64_t jitter_window_us =
                    interval_us * config.jitter_percentage / 100U;
                uint64_t jitter_us = jitter_window_us == 0U
                                         ? 0U
                                         : (uint64_t)esp_random() %
                                               jitter_window_us;
                pending_at_us = esp_timer_get_time() + (int64_t)jitter_us;
                pending_revision = revision;
                pending = true;
            }
        }

        int64_t monotonic_now = esp_timer_get_time();
        if (pending && monotonic_now >= pending_at_us) {
            bool current_enabled = false;
            uint32_t current_revision = 0U;
            jetkvm_control_jiggler_config_t current_config = {0};
            if (jetkvm_hid_profiles_get_jiggler_snapshot(
                    &current_enabled, &current_config,
                    &current_revision) == ESP_OK &&
                current_enabled && current_revision == pending_revision) {
                int64_t last_input_us =
                    jetkvm_hid_input_last_user_input_us();
                int64_t inactivity_us =
                    (int64_t)current_config.inactivity_limit_seconds *
                    INT64_C(1000000);
                if (last_input_us >= 0 &&
                    monotonic_now - last_input_us >= inactivity_us) {
                    int8_t dx = random_delta();
                    int8_t dy = random_delta();
                    esp_err_t error =
                        jetkvm_hid_input_submit_local_relative_mouse(dx, dy);
                    if (error == ESP_OK) {
                        ESP_LOGD(TAG, "jiggled mouse dx=%d dy=%d revision=%" PRIu32,
                                 dx, dy, current_revision);
                    } else {
                        ESP_LOGW(TAG, "mouse jiggle unavailable: %s",
                                 esp_err_to_name(error));
                    }
                }
            }
            pending = false;
        }
        vTaskDelay(pdMS_TO_TICKS(JIGGLER_POLL_MS));
    }
}

esp_err_t jetkvm_hid_profiles_start_jiggler(void)
{
    if (!jetkvm_hid_profiles_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_jiggler_lock);
    if (s_jiggler_task != NULL) {
        portEXIT_CRITICAL(&s_jiggler_lock);
        return ESP_OK;
    }
    if (s_jiggler_starting) {
        portEXIT_CRITICAL(&s_jiggler_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_jiggler_starting = true;
    portEXIT_CRITICAL(&s_jiggler_lock);

    /* The task reads the schedule and queues a mouse report; it writes no
     * flash, so its stack is in PSRAM and leaves internal DMA RAM to TLS. */
    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreateWithCaps(
        jiggler_task, JIGGLER_TASK_NAME, JIGGLER_TASK_STACK_SIZE, NULL,
        JIGGLER_TASK_PRIORITY, &task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&s_jiggler_lock);
    s_jiggler_starting = false;
    if (created == pdPASS) {
        s_jiggler_task = task;
    }
    portEXIT_CRITICAL(&s_jiggler_lock);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
