/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_cloud_client.h"
#include "jetkvm_oidc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool started;
    bool configured;
    bool time_ready;
    bool client_running;
    uint32_t config_generation;
    uint64_t rebuilds;
    uint64_t start_failures;
    uint64_t destroy_failures;
    jetkvm_cloud_client_stats_t client;
} jetkvm_cloud_service_stats_t;

/**
 * Start the config-driven cloud worker after WebRTC transport is ready.
 * The validator is borrowed and must remain valid until stop() returns.
 */
esp_err_t jetkvm_cloud_service_start(jetkvm_oidc_validator_t *oidc);
/** Stop WSS, its controlled WebRTC session, and all identity callbacks. */
esp_err_t jetkvm_cloud_service_stop(void);
esp_err_t jetkvm_cloud_service_get_stats(
    jetkvm_cloud_service_stats_t *stats);

#ifdef __cplusplus
}
#endif
