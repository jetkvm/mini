/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "host_fakes.h"

static void assert_service_running(uint32_t generation,
                                   uint64_t minimum_rebuilds)
{
    for (unsigned attempt = 0U; attempt < 500U; ++attempt) {
        jetkvm_cloud_service_stats_t stats = { 0 };
        assert(jetkvm_cloud_service_get_stats(&stats) == ESP_OK);
        if (stats.started && stats.configured && stats.time_ready &&
            stats.client_running && stats.config_generation == generation &&
            stats.rebuilds >= minimum_rebuilds) {
            return;
        }
        vTaskDelay(1U);
    }
    assert(!"cloud service did not publish the expected running snapshot");
}

static void *stop_service(void *unused)
{
    (void)unused;
    assert(jetkvm_cloud_service_stop() == ESP_OK);
    return NULL;
}

typedef struct {
    jetkvm_oidc_validator_t *validator;
    esp_err_t result;
    atomic_bool done;
} start_call_t;

typedef struct {
    esp_err_t result;
    atomic_bool done;
} stop_call_t;

static void *start_service_call(void *opaque)
{
    start_call_t *call = opaque;
    call->result = jetkvm_cloud_service_start(call->validator);
    atomic_store_explicit(&call->done, true, memory_order_release);
    return NULL;
}

static void *stop_service_call(void *opaque)
{
    stop_call_t *call = opaque;
    call->result = jetkvm_cloud_service_stop();
    atomic_store_explicit(&call->done, true, memory_order_release);
    return NULL;
}

int main(void)
{
    jetkvm_oidc_validator_t validator = { .marker = 0x1234U };
    fake_memory_reset();
    fake_client_reset();
    fake_state_reset(false, NULL, NULL);
    fake_time_set_ready(false);

    assert(jetkvm_cloud_service_start(&validator) == ESP_OK);
    assert(!fake_client_wait_counts(1U, 0U, 30U));

    uint32_t generation = fake_state_replace(
        true, "token-1", "client-1:subject-1", true);
    assert(!fake_client_wait_counts(1U, 0U, 30U));
    fake_time_set_ready(true);
    assert(fake_client_wait_counts(1U, 0U, 500U));
    assert(fake_client_last_token_is("token-1"));
    assert_service_running(generation, 1U);
    static const char token_1[] = "ok:client-1:subject-1";
    static const char wrong_identity[] = "ok:client-1:other";
    assert(fake_client_validate_token_status(token_1, strlen(token_1)) ==
           JETKVM_CLOUD_IDENTITY_VALID);
    assert(fake_client_validate_token(token_1, strlen(token_1)));
    assert(fake_client_validate_token_status(
               wrong_identity, strlen(wrong_identity)) ==
           JETKVM_CLOUD_IDENTITY_INVALID);
    assert(!fake_client_validate_token(
        wrong_identity, strlen(wrong_identity)));
    assert(fake_memory_active_count() == 0U);
    assert(fake_memory_reserve_count() == fake_memory_release_count());

    static const char jwks_unavailable[] = "jwks-unavailable";
    assert(fake_client_validate_token_status(
               jwks_unavailable, strlen(jwks_unavailable)) ==
           JETKVM_CLOUD_IDENTITY_RETRYABLE);
    assert(fake_memory_active_count() == 0U);
    assert(fake_memory_reserve_count() == fake_memory_release_count());

    unsigned memory_reserves = fake_memory_reserve_count();
    unsigned memory_releases = fake_memory_release_count();
    fake_memory_set_allowed(false);
    assert(fake_client_validate_token_status(token_1, strlen(token_1)) ==
           JETKVM_CLOUD_IDENTITY_RETRYABLE);
    assert(fake_memory_reserve_count() == memory_reserves + 1U);
    assert(fake_memory_release_count() == memory_releases);
    assert(fake_memory_active_count() == 0U);
    fake_memory_set_allowed(true);

    generation = fake_state_replace(
        true, "token-2", "client-2:subject-2", true);
    assert(fake_client_wait_counts(2U, 1U, 500U));
    assert(fake_client_last_token_is("token-2"));
    assert_service_running(generation, 2U);
    static const char token_2[] = "ok:client-2:subject-2";
    assert(fake_client_validate_token(token_2, strlen(token_2)));
    assert(!fake_client_validate_token(token_1, strlen(token_1)));

    /* A published generation must invalidate the old identity even before
     * the state observer is dispatched. */
    generation = fake_state_replace(
        true, "token-3", "client-3:subject-3", false);
    assert(!fake_client_validate_token(token_2, strlen(token_2)));
    fake_state_emit_current();
    assert(fake_client_wait_counts(3U, 2U, 500U));
    assert(fake_client_last_token_is("token-3"));
    assert_service_running(generation, 3U);

    /* If config changes while cloud_client_start() is blocked, that WSS is
     * destroyed before publication and only the newest snapshot survives. */
    fake_client_block_next_start();
    (void)fake_state_replace(
        true, "token-4", "client-4:subject-4", true);
    assert(fake_client_wait_start_entered(500U));
    generation = fake_state_replace(
        true, "token-5", "client-5:subject-5", true);
    fake_client_release_start();
    assert(fake_client_wait_counts(5U, 4U, 1000U));
    assert(fake_client_last_token_is("token-5"));
    assert_service_running(generation, 4U);
    static const char token_5[] = "ok:client-5:subject-5";
    assert(fake_client_validate_token(token_5, strlen(token_5)));

    /* stop() invalidates identity immediately, then waits for a start already
     * in progress before destroying the client and returning. */
    fake_client_block_next_start();
    (void)fake_state_replace(
        true, "token-6", "client-6:subject-6", true);
    assert(fake_client_wait_start_entered(500U));
    static const char token_6[] = "ok:client-6:subject-6";
    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stop_service, NULL) == 0);
    bool invalidated = false;
    for (unsigned attempt = 0U; attempt < 500U; ++attempt) {
        if (!fake_client_validate_token(token_6, strlen(token_6))) {
            invalidated = true;
            break;
        }
        vTaskDelay(1U);
    }
    assert(invalidated);
    fake_client_release_start();
    assert(pthread_join(stopper, NULL) == 0);
    assert(fake_client_start_count() >= 6U);
    assert(fake_client_destroy_count() >= 6U);
    assert(!fake_client_validate_token(token_5, strlen(token_5)));

    /* Simulate a callback captured immediately before unsubscribe. Service
     * resources remain valid and a later start snapshots authoritative state. */
    fake_state_emit_late();
    assert(jetkvm_cloud_service_start(&validator) == ESP_OK);
    assert(fake_client_wait_counts(7U, 6U, 500U));
    assert(fake_client_last_token_is("token-6"));
    assert(jetkvm_cloud_service_stop() == ESP_OK);
    assert(fake_client_destroy_count() >= 7U);

    /* Hold xTaskCreate() after the worker has launched. The priority-5 worker
     * and a concurrent stop() must both block on the start-resolved event;
     * neither may busy-yield while the lower-priority starter is paused. */
    fake_state_reset(false, NULL, NULL);
    fake_time_set_ready(true);
    fake_task_block_next_create(false);
    start_call_t gated_start = {
        .validator = &validator,
        .result = ESP_FAIL,
    };
    atomic_init(&gated_start.done, false);
    pthread_t starter;
    assert(pthread_create(&starter, NULL, start_service_call,
                          &gated_start) == 0);
    assert(fake_task_wait_create_entered(500U));
    assert(fake_event_wait_for_waiters(BIT2, 1U, 500U));

    stop_call_t racing_stop = { .result = ESP_FAIL };
    atomic_init(&racing_stop.done, false);
    pthread_t start_stopper;
    assert(pthread_create(&start_stopper, NULL, stop_service_call,
                          &racing_stop) == 0);
    assert(fake_event_wait_for_waiters(BIT2, 2U, 500U));
    assert(!atomic_load_explicit(&gated_start.done, memory_order_acquire));
    assert(!atomic_load_explicit(&racing_stop.done, memory_order_acquire));
    fake_task_release_create();
    assert(pthread_join(starter, NULL) == 0);
    assert(pthread_join(start_stopper, NULL) == 0);
    assert(gated_start.result == ESP_OK);
    assert(racing_stop.result == ESP_OK);

    /* A task-creation failure resolves the same gate. stop() racing with that
     * failure waits without spinning and returns once start() rolls back. */
    fake_task_block_next_create(true);
    start_call_t failed_start = {
        .validator = &validator,
        .result = ESP_FAIL,
    };
    atomic_init(&failed_start.done, false);
    assert(pthread_create(&starter, NULL, start_service_call,
                          &failed_start) == 0);
    assert(fake_task_wait_create_entered(500U));

    stop_call_t failed_start_stop = { .result = ESP_FAIL };
    atomic_init(&failed_start_stop.done, false);
    assert(pthread_create(&start_stopper, NULL, stop_service_call,
                          &failed_start_stop) == 0);
    assert(fake_event_wait_for_waiters(BIT2, 1U, 500U));
    assert(!atomic_load_explicit(&failed_start.done, memory_order_acquire));
    assert(!atomic_load_explicit(&failed_start_stop.done,
                                 memory_order_acquire));
    fake_task_release_create();
    assert(pthread_join(starter, NULL) == 0);
    assert(pthread_join(start_stopper, NULL) == 0);
    assert(failed_start.result == ESP_ERR_NO_MEM);
    assert(failed_start_stop.result == ESP_OK);

    /* Deregistration destroys a connected client; the stats must not keep
     * reporting its WebSocket as connected. */
    fake_state_reset(true, "token-7", "client-7:subject-7");
    unsigned starts = fake_client_start_count();
    assert(jetkvm_cloud_service_start(&validator) == ESP_OK);
    assert(fake_client_wait_counts(starts + 1U, 0U, 500U));
    jetkvm_cloud_service_stats_t stats = { 0 };
    for (unsigned attempt = 0U; attempt < 500U && !stats.client.connected;
         ++attempt) {
        assert(jetkvm_cloud_service_get_stats(&stats) == ESP_OK);
        vTaskDelay(1U);
    }
    assert(stats.client.connected);
    unsigned destroys = fake_client_destroy_count();
    (void)fake_state_replace(false, NULL, NULL, true);
    assert(fake_client_wait_counts(0U, destroys + 1U, 500U));
    assert(jetkvm_cloud_service_get_stats(&stats) == ESP_OK);
    assert(!stats.client.connected);
    assert(jetkvm_cloud_service_stop() == ESP_OK);

    puts("jetkvm_cloud_service lifecycle tests passed");
    return 0;
}
