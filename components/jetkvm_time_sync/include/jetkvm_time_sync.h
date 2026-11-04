/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "jetkvm_time_sync_servers.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set the NTP query order. Takes effect immediately when the client is
 * running: the next request goes to the first server in the new list.
 */
esp_err_t jetkvm_time_sync_configure(const jetkvm_time_sync_config_t *config);

/** Start the nonblocking SNTP client used by TLS and OIDC validation. */
esp_err_t jetkvm_time_sync_start(void);

/** Wait for a trusted wall-clock sample without stopping local video. */
esp_err_t jetkvm_time_sync_wait(TickType_t timeout);

bool jetkvm_time_sync_ready(void);
void jetkvm_time_sync_stop(void);

#ifdef __cplusplus
}
#endif
