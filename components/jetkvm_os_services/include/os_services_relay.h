/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The OS Services update relay on the device. A worker task, started when
 * there is work and gone when there is none, refreshes the release indexes
 * and downloads a package into two 64 KiB PSRAM buffers; the OS Services
 * task, which alone writes to the sender, relays the buffers within the
 * sender's credit (osvc_relay_service). Never alongside a firmware install.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "os_services_session.h"

/* service_task is woken when the relay has work for it. */
esp_err_t osvc_relay_init(TaskHandle_t service_task);

/*
 * The OS Services task's turn, once per loop: follows the sender, relays the
 * queued buffers, takes the update's outcome and schedules index refreshes
 * and automatic updates. Returns true when the browser's state changed.
 */
bool osvc_relay_service(osvc_session_t *session);

/* Changes whenever the browser's guestUpdate changes. */
uint32_t osvc_relay_generation(void);

/* The browser's guestUpdateStart: NULL, or why no update starts. */
const char *osvc_relay_start(void);

/* Adds "guestUpdate" to the video state. Does not block for long. */
bool osvc_relay_add_state(cJSON *object);
