/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_cloud_service_policy.h"

#include <string.h>

bool jetkvm_cloud_service_policy_snapshot_current(
    uint32_t snapshot_epoch, uint32_t current_epoch,
    bool service_running, bool stop_requested)
{
    return service_running && !stop_requested &&
           snapshot_epoch == current_epoch;
}

bool jetkvm_cloud_service_policy_offer_current(
    uint32_t active_epoch, uint32_t current_epoch,
    uint32_t active_generation, bool service_running,
    bool stop_requested, bool client_owned)
{
    return service_running && !stop_requested && client_owned &&
           active_generation != 0U && active_epoch == current_epoch;
}

bool jetkvm_cloud_service_policy_identity_equal(
    const char *left, const char *right, size_t capacity)
{
    if (!left || !right || capacity == 0U) {
        return false;
    }
    unsigned difference = 0U;
    bool left_terminated = false;
    bool right_terminated = false;
    for (size_t i = 0; i < capacity; ++i) {
        unsigned char a = left_terminated ? 0U : (unsigned char)left[i];
        unsigned char b = right_terminated ? 0U : (unsigned char)right[i];
        difference |= (unsigned)(a ^ b);
        left_terminated = left_terminated || a == 0U;
        right_terminated = right_terminated || b == 0U;
    }
    return difference == 0U && left_terminated && right_terminated;
}

bool jetkvm_cloud_service_policy_token_shape_valid(
    const char *token, size_t token_length, size_t maximum_length)
{
    return token && token_length > 0U && token_length <= maximum_length &&
           token[token_length] == '\0' &&
           memchr(token, '\0', token_length) == NULL;
}
