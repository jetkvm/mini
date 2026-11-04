/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_cloud_service_policy.h"

static void test_snapshot_freshness(void)
{
    assert(jetkvm_cloud_service_policy_snapshot_current(
        7U, 7U, true, false));
    assert(!jetkvm_cloud_service_policy_snapshot_current(
        7U, 8U, true, false));
    assert(!jetkvm_cloud_service_policy_snapshot_current(
        7U, 7U, false, false));
    assert(!jetkvm_cloud_service_policy_snapshot_current(
        7U, 7U, true, true));

    /* Epoch equality remains valid across uint32_t wrap. */
    assert(jetkvm_cloud_service_policy_snapshot_current(
        0U, 0U, true, false));
    assert(!jetkvm_cloud_service_policy_snapshot_current(
        UINT32_MAX, 0U, true, false));
}

static void test_offer_gate(void)
{
    assert(jetkvm_cloud_service_policy_offer_current(
        9U, 9U, 4U, true, false, true));
    assert(!jetkvm_cloud_service_policy_offer_current(
        9U, 10U, 4U, true, false, true));
    assert(!jetkvm_cloud_service_policy_offer_current(
        9U, 9U, 0U, true, false, true));
    assert(!jetkvm_cloud_service_policy_offer_current(
        9U, 9U, 4U, false, false, true));
    assert(!jetkvm_cloud_service_policy_offer_current(
        9U, 9U, 4U, true, true, true));
    assert(!jetkvm_cloud_service_policy_offer_current(
        9U, 9U, 4U, true, false, false));
}

static void test_identity_comparison(void)
{
    char expected[32] = "client:subject";
    char same[32] = "client:subject";
    char wrong_subject[32] = "client:other";
    char wrong_audience[32] = "other:subject";
    char unterminated[32];
    memset(unterminated, 'x', sizeof(unterminated));

    assert(jetkvm_cloud_service_policy_identity_equal(
        expected, same, sizeof(expected)));
    assert(!jetkvm_cloud_service_policy_identity_equal(
        expected, wrong_subject, sizeof(expected)));
    assert(!jetkvm_cloud_service_policy_identity_equal(
        expected, wrong_audience, sizeof(expected)));
    assert(!jetkvm_cloud_service_policy_identity_equal(
        expected, unterminated, sizeof(expected)));
    assert(!jetkvm_cloud_service_policy_identity_equal(
        NULL, same, sizeof(expected)));
    assert(!jetkvm_cloud_service_policy_identity_equal(
        expected, same, 0U));
}

static void test_token_shape(void)
{
    char valid[] = "aaa.bbb.ccc";
    char embedded_nul[] = { 'a', 'a', '\0', '.', 'b', '\0' };
    char not_terminated[] = { 'a', 'b', 'c', 'd' };

    assert(jetkvm_cloud_service_policy_token_shape_valid(
        valid, strlen(valid), 64U));
    assert(!jetkvm_cloud_service_policy_token_shape_valid(
        embedded_nul, 5U, 64U));
    assert(!jetkvm_cloud_service_policy_token_shape_valid(
        not_terminated, 3U, 64U));
    assert(!jetkvm_cloud_service_policy_token_shape_valid(
        valid, strlen(valid), 4U));
    assert(!jetkvm_cloud_service_policy_token_shape_valid(
        valid, 0U, 64U));
    assert(!jetkvm_cloud_service_policy_token_shape_valid(
        NULL, 1U, 64U));
}

int main(void)
{
    test_snapshot_freshness();
    test_offer_gate();
    test_identity_comparison();
    test_token_shape();
    puts("jetkvm_cloud_service policy tests passed");
    return 0;
}
