/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool jetkvm_cloud_service_policy_snapshot_current(
    uint32_t snapshot_epoch, uint32_t current_epoch,
    bool service_running, bool stop_requested);

bool jetkvm_cloud_service_policy_offer_current(
    uint32_t active_epoch, uint32_t current_epoch,
    uint32_t active_generation, bool service_running,
    bool stop_requested, bool client_owned);

bool jetkvm_cloud_service_policy_identity_equal(
    const char *left, const char *right, size_t capacity);

bool jetkvm_cloud_service_policy_token_shape_valid(
    const char *token, size_t token_length, size_t maximum_length);
