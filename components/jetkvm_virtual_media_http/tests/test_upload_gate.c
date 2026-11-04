/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* Runs http_upload_gate.c with real threads: a card writer, a password
 * change that holds the access mutex while it revokes, and the upload's
 * finish step that waits for that mutex. */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_timer.h"
#include "http_upload_gate.h"

static atomic_uint s_epoch = 1U;
static pthread_mutex_t s_access_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_server;

static bool epoch_valid(uint32_t epoch)
{
    return epoch == atomic_load(&s_epoch);
}

typedef struct {
    int64_t write_us;
    atomic_bool in_write;
    atomic_bool write_ended;
    atomic_bool second_write_allowed;
} writer_t;

/* One card write, then the writer's next turn. */
static void *writer_main(void *argument)
{
    writer_t *writer = argument;
    assert(http_upload_gate_write_begin());
    atomic_store(&writer->in_write, true);
    usleep((useconds_t)writer->write_us);
    atomic_store(&writer->write_ended, true);
    http_upload_gate_write_end();
    atomic_store(&writer->second_write_allowed,
                 http_upload_gate_write_begin());
    if (atomic_load(&writer->second_write_allowed)) {
        http_upload_gate_write_end();
    }
    return NULL;
}

typedef struct {
    atomic_bool waiting;
    atomic_bool finished;
    bool cancelled_seen;
} finisher_t;

/* The upload's finish step: it takes the access mutex, as the firmware's
 * jetkvm_access_lock_epoch() does. */
static void *finisher_main(void *argument)
{
    finisher_t *finisher = argument;
    atomic_store(&finisher->waiting, true);
    pthread_mutex_lock(&s_access_mutex);
    finisher->cancelled_seen = http_upload_gate_cancelled();
    pthread_mutex_unlock(&s_access_mutex);
    atomic_store(&finisher->finished, true);
    return NULL;
}

static void wait_until(atomic_bool *flag)
{
    while (!atomic_load(flag)) {
        usleep(100);
    }
}

/* A password change during a card write returns only after that write has
 * ended, and the writer starts no further write. The finish step waits on
 * the access mutex all the while, and the revocation does not wait for it. */
static void test_revoke_waits_for_write_not_for_finish(void)
{
    assert(http_upload_gate_open(&s_server, atomic_load(&s_epoch),
                                 epoch_valid));
    writer_t writer = {.write_us = 80000};
    finisher_t finisher = {0};
    pthread_t writer_thread;
    pthread_t finisher_thread;
    assert(pthread_create(&writer_thread, NULL, writer_main, &writer) == 0);
    wait_until(&writer.in_write);

    pthread_mutex_lock(&s_access_mutex);
    assert(pthread_create(&finisher_thread, NULL, finisher_main,
                          &finisher) == 0);
    wait_until(&finisher.waiting);
    const int64_t started = esp_timer_get_time();
    atomic_fetch_add(&s_epoch, 1U);
    assert(http_upload_gate_revoke(HTTP_UPLOAD_GATE_WRITE_WAIT_US) == ESP_OK);
    const int64_t waited = esp_timer_get_time() - started;
    assert(atomic_load(&writer.write_ended));
    assert(!atomic_load(&finisher.finished));
    assert(waited < 1000000);
    pthread_mutex_unlock(&s_access_mutex);

    assert(pthread_join(writer_thread, NULL) == 0);
    assert(pthread_join(finisher_thread, NULL) == 0);
    assert(!atomic_load(&writer.second_write_allowed));
    assert(finisher.cancelled_seen);
    http_upload_gate_close();
}

/* A write that the card does not finish holds the revocation no longer than
 * the wait it was given. */
static void test_revoke_wait_is_bounded(void)
{
    assert(http_upload_gate_open(&s_server, atomic_load(&s_epoch),
                                 epoch_valid));
    writer_t writer = {.write_us = 400000};
    pthread_t writer_thread;
    assert(pthread_create(&writer_thread, NULL, writer_main, &writer) == 0);
    wait_until(&writer.in_write);

    const int64_t started = esp_timer_get_time();
    atomic_fetch_add(&s_epoch, 1U);
    assert(http_upload_gate_revoke(50000) == ESP_ERR_TIMEOUT);
    const int64_t waited = esp_timer_get_time() - started;
    assert(waited >= 50000 && waited < 300000);
    assert(!atomic_load(&writer.write_ended));
    /* A second revocation, as the expiry of the revoked session, does not
     * wait for that write again. */
    const int64_t again = esp_timer_get_time();
    assert(http_upload_gate_revoke(50000) == ESP_ERR_TIMEOUT);
    assert(esp_timer_get_time() - again < 20000);

    assert(pthread_join(writer_thread, NULL) == 0);
    assert(!atomic_load(&writer.second_write_allowed));
    http_upload_gate_close();
}

/* The upload request is not admitted under the access mutex, so a
 * revocation can run before it opens the gate with the old epoch. Its first
 * write is refused. */
static void test_upload_opened_after_revoke_writes_nothing(void)
{
    const uint32_t admitted = atomic_load(&s_epoch);
    atomic_fetch_add(&s_epoch, 1U);
    assert(http_upload_gate_revoke(HTTP_UPLOAD_GATE_WRITE_WAIT_US) == ESP_OK);
    assert(http_upload_gate_open(&s_server, admitted, epoch_valid));
    assert(!http_upload_gate_write_begin());
    http_upload_gate_close();
}

/* A revocation that leaves the upload's epoch valid, such as the expiry of
 * a stale browser session, neither cancels the upload nor waits. */
static void test_current_upload_keeps_writing(void)
{
    assert(http_upload_gate_open(&s_server, atomic_load(&s_epoch),
                                 epoch_valid));
    assert(http_upload_gate_write_begin());
    const int64_t started = esp_timer_get_time();
    assert(http_upload_gate_revoke(HTTP_UPLOAD_GATE_WRITE_WAIT_US) == ESP_OK);
    assert(esp_timer_get_time() - started < 50000);
    http_upload_gate_write_end();
    assert(!http_upload_gate_cancelled());
    assert(http_upload_gate_write_begin());
    http_upload_gate_write_end();
    http_upload_gate_close();
}

/* One upload holds the gate; the server drain cancels it and reports it
 * until it closes. */
static void test_one_upload_and_drain(void)
{
    int other_server;
    assert(http_upload_gate_open(&s_server, atomic_load(&s_epoch),
                                 epoch_valid));
    assert(!http_upload_gate_open(&s_server, atomic_load(&s_epoch),
                                  epoch_valid));
    assert(!http_upload_gate_cancel_server(&other_server));
    assert(!http_upload_gate_cancelled());
    assert(http_upload_gate_cancel_server(&s_server));
    assert(http_upload_gate_cancelled());
    assert(!http_upload_gate_write_begin());
    http_upload_gate_close();
    assert(!http_upload_gate_cancel_server(&s_server));
    assert(http_upload_gate_open(&s_server, atomic_load(&s_epoch),
                                 epoch_valid));
    assert(!http_upload_gate_cancelled());
    http_upload_gate_close();
}

int main(void)
{
    test_revoke_waits_for_write_not_for_finish();
    test_revoke_wait_is_bounded();
    test_upload_opened_after_revoke_writes_nothing();
    test_current_upload_keeps_writing();
    test_one_upload_and_drain();
    puts("HTTP upload gate: PASS");
    return 0;
}
