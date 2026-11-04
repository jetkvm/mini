/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* The longest time one SD write command can run before the SD driver fails
 * it: the card's busy wait before the transfer
 * (SDMMC_READY_FOR_DATA_TIMEOUT_US, 5 s) plus the write command's own
 * timeout (SDMMC_WRITE_CMD_TIMEOUT_MS, 5 s). http_upload_gate.c checks both
 * values against ESP-IDF. One upload write is one such command on a working
 * card; a write that is still running after this wait is on a card that the
 * driver is failing, and it can issue further commands before it returns. */
#define HTTP_UPLOAD_GATE_WRITE_WAIT_US INT64_C(10000000)

/* The state that one HTTP upload shares between its request task, its card
 * writer task and the revocation of local access. The HTTP server runs one
 * upload at a time, so there is one gate.
 *
 * A password change, login or setup starts a new access epoch and then calls
 * http_upload_gate_revoke() while it holds the access mutex. Revocation
 * cancels an upload admitted under an older epoch and waits until its card
 * write in progress has returned. After that, the writer starts no further
 * write: http_upload_gate_write_begin() refuses a cancelled upload and an
 * upload whose epoch is no longer current. So when the password change
 * returns, no write of the revoked upload runs, and the writer needs no
 * access mutex for its writes. */

/* Opens the gate for an upload admitted under access_epoch. Returns false
 * while another upload holds the gate. */
bool http_upload_gate_open(void *server, uint32_t access_epoch,
                           bool (*epoch_valid)(uint32_t epoch));
void http_upload_gate_close(void);

/* True when revocation or the server drain has cancelled the open upload. */
bool http_upload_gate_cancelled(void);

/* Cancels the upload of this server. Returns true while that upload still
 * holds the gate. */
bool http_upload_gate_cancel_server(void *server);

/* The card writer calls write_begin before each write and runs the write
 * only when it returns true; it then calls write_end after the write. */
bool http_upload_gate_write_begin(void);
void http_upload_gate_write_end(void);

/* Cancels the open upload if its access epoch is no longer valid, and waits
 * until its card write in progress has returned. It never waits for the
 * upload to end and takes no lock that the upload task can hold while it
 * waits, so the caller may hold the access mutex. Returns ESP_ERR_TIMEOUT
 * when the write is still running after wait_us; after that, a revocation
 * of the same upload returns ESP_ERR_TIMEOUT at once while the write runs. */
esp_err_t http_upload_gate_revoke(int64_t wait_us);
