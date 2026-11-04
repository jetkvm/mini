/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "http_upload_gate.h"

#include <stdatomic.h>
#include <stddef.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef ESP_PLATFORM
#include "esp_private/sdmmc_common.h"
_Static_assert(HTTP_UPLOAD_GATE_WRITE_WAIT_US ==
                   (int64_t)SDMMC_READY_FOR_DATA_TIMEOUT_US +
                       (int64_t)SDMMC_WRITE_CMD_TIMEOUT_MS * 1000,
               "the revocation wait must match the SD driver's write timeout");
#endif

typedef struct {
    portMUX_TYPE lock;
    bool open;
    uint32_t generation;
    void *server;
    uint32_t access_epoch;
    bool (*epoch_valid)(uint32_t epoch);
    atomic_bool cancelled;
    bool card_write_active;
    /* A revocation's wait for this upload's write has run out once. */
    bool wait_expired;
} http_upload_gate_t;

static http_upload_gate_t s_gate = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

bool http_upload_gate_open(void *server, uint32_t access_epoch,
                           bool (*epoch_valid)(uint32_t epoch))
{
    portENTER_CRITICAL(&s_gate.lock);
    const bool opened = !s_gate.open;
    if (opened) {
        s_gate.open = true;
        s_gate.generation++;
        s_gate.server = server;
        s_gate.access_epoch = access_epoch;
        s_gate.epoch_valid = epoch_valid;
        atomic_store(&s_gate.cancelled, false);
        s_gate.card_write_active = false;
        s_gate.wait_expired = false;
    }
    portEXIT_CRITICAL(&s_gate.lock);
    return opened;
}

void http_upload_gate_close(void)
{
    portENTER_CRITICAL(&s_gate.lock);
    s_gate.open = false;
    s_gate.server = NULL;
    s_gate.card_write_active = false;
    portEXIT_CRITICAL(&s_gate.lock);
}

bool http_upload_gate_cancelled(void)
{
    return atomic_load(&s_gate.cancelled);
}

bool http_upload_gate_cancel_server(void *server)
{
    portENTER_CRITICAL(&s_gate.lock);
    const bool pending = s_gate.open && s_gate.server == server;
    if (pending) {
        atomic_store(&s_gate.cancelled, true);
    }
    portEXIT_CRITICAL(&s_gate.lock);
    return pending;
}

bool http_upload_gate_write_begin(void)
{
    /* The epoch check covers a revocation that ran before this upload opened
     * the gate: the upload request is not admitted under the access mutex,
     * so it can still carry the epoch that the revocation ended. */
    portENTER_CRITICAL(&s_gate.lock);
    const bool allowed = s_gate.open && !atomic_load(&s_gate.cancelled) &&
                         s_gate.epoch_valid(s_gate.access_epoch);
    s_gate.card_write_active = allowed;
    portEXIT_CRITICAL(&s_gate.lock);
    return allowed;
}

void http_upload_gate_write_end(void)
{
    portENTER_CRITICAL(&s_gate.lock);
    s_gate.card_write_active = false;
    portEXIT_CRITICAL(&s_gate.lock);
}

static bool write_in_progress(uint32_t generation)
{
    portENTER_CRITICAL(&s_gate.lock);
    const bool writing = s_gate.open && s_gate.generation == generation &&
                         s_gate.card_write_active;
    portEXIT_CRITICAL(&s_gate.lock);
    return writing;
}

esp_err_t http_upload_gate_revoke(int64_t wait_us)
{
    portENTER_CRITICAL(&s_gate.lock);
    const bool revoked = s_gate.open &&
                         !s_gate.epoch_valid(s_gate.access_epoch);
    if (revoked) {
        atomic_store(&s_gate.cancelled, true);
    }
    const uint32_t generation = s_gate.generation;
    const bool expired = s_gate.wait_expired;
    portEXIT_CRITICAL(&s_gate.lock);
    if (!revoked) {
        return ESP_OK;
    }
    /* A write that outlasted one full wait is on a card that the driver is
     * failing. A later revocation, such as the expiry of the revoked
     * browser session on the peer task, does not wait for it again. */
    const int64_t deadline = esp_timer_get_time() + (expired ? 0 : wait_us);
    while (write_in_progress(generation)) {
        if (esp_timer_get_time() >= deadline) {
            portENTER_CRITICAL(&s_gate.lock);
            if (s_gate.generation == generation) {
                s_gate.wait_expired = true;
            }
            portEXIT_CRITICAL(&s_gate.lock);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}
