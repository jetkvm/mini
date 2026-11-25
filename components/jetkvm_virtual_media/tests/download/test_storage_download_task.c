/* SPDX-License-Identifier: MIT */
/*
 * The download task of storage_download.c against a fake server and a fake
 * TF card. The task runs in the test's own call, so a cancel comes from the
 * fake server's read, at a chosen point of the transfer.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_storage_download.h"
#include "jetkvm_virtual_media.h"

#define BODY_BYTES (3U * 64U * 1024U + 100U)
#define READ_BYTES (16U * 1024U)

/* The fake server. */
static struct {
    int status;
    uint64_t sent;
    unsigned reads;
    /* Ask for a cancel in read number cancel_at_read (1 = the first). */
    unsigned cancel_at_read;
    /* Ask for a cancel in the read that sends the last byte. */
    bool cancel_at_last_byte;
    /* Ask for a cancel while the request connects. */
    bool cancel_at_open;
    /* The connection fails in this read. */
    unsigned fail_at_read;
} server;

/* The fake TF card. */
static struct {
    unsigned starts;
    unsigned claims;
    uint64_t written;
    unsigned finishes;
    unsigned aborts;
    unsigned discards;
    esp_err_t discard_result;
    /* A writer lease is held. */
    bool claimed;
    /* An earlier download left os.iso.incomplete. */
    bool partial;
    unsigned deletes;
    esp_err_t delete_result;
} card;

static TaskFunction_t s_task;
static void *s_task_argument;

static void reset(void)
{
    memset(&server, 0, sizeof(server));
    server.status = 200;
    memset(&card, 0, sizeof(card));
    card.discard_result = ESP_OK;
    card.delete_result = ESP_OK;
}

/* Start a download and run its task to the end. */
static jetkvm_storage_download_state_t run_download(void)
{
    char error[128] = "";
    assert(jetkvm_storage_download_start("https://example.test/os.iso", "os.iso", error,
                                         sizeof(error)) == ESP_OK);
    assert(s_task != NULL);
    TaskFunction_t task = s_task;
    s_task = NULL;
    task(s_task_argument);
    jetkvm_storage_download_state_t state;
    jetkvm_storage_download_get_state(&state);
    return state;
}

static void test_cancel_during_transfer(void)
{
    reset();
    /* Reads 1 to 4 fill the first 64 KiB block; the cancel comes in read 6. */
    server.cancel_at_read = 6U;
    jetkvm_storage_download_state_t state = run_download();
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_CANCELLED);
    assert(state.error[0] == '\0');
    /* No read after the cancel, and no write of the unfinished block. */
    assert(server.reads == 6U);
    assert(state.bytes == 64U * 1024U && card.written == 64U * 1024U);
    /* The partial file is deleted, not kept for a resume. */
    assert(card.discards == 1U && card.aborts == 0U && card.finishes == 0U);
    assert(!card.claimed);
    /* No download runs any more: a cancel does nothing, a new one starts. */
    assert(!jetkvm_storage_download_cancel());
    reset();
    state = run_download();
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_DONE);
}

static void test_cancel_after_last_byte(void)
{
    reset();
    server.cancel_at_last_byte = true;
    jetkvm_storage_download_state_t state = run_download();
    /* The file is complete: it stays under its name. */
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_DONE);
    assert(state.bytes == BODY_BYTES && state.total == BODY_BYTES);
    assert(card.finishes == 1U && card.discards == 0U && card.aborts == 0U);
}

static void test_cancel_while_connecting(void)
{
    reset();
    server.cancel_at_open = true;
    server.status = 404;
    jetkvm_storage_download_state_t state = run_download();
    /* The cancel wins over the server's error; no file was created. */
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_CANCELLED);
    assert(state.error[0] == '\0');
    assert(card.starts == 0U && card.discards == 0U && card.aborts == 0U);

    reset();
    server.cancel_at_open = true;
    state = run_download();
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_CANCELLED);
    assert(card.starts == 0U && server.reads == 0U);
}

static void test_cancel_while_connecting_deletes_older_partial(void)
{
    /* The cancel comes before the task holds the card, so no session can
     * delete the file that a failed download left. */
    reset();
    server.cancel_at_open = true;
    card.partial = true;
    jetkvm_storage_download_state_t state = run_download();
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_CANCELLED);
    assert(state.error[0] == '\0');
    assert(!card.partial && card.starts == 0U);

    /* A mounted image refuses the delete: the state names the file. */
    reset();
    server.cancel_at_open = true;
    card.partial = true;
    card.delete_result = ESP_ERR_INVALID_STATE;
    state = run_download();
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_CANCELLED);
    assert(strstr(state.error, "Could not delete the partial os.iso") == state.error);
    assert(card.partial && card.deletes == 1U);
}

static void test_failure_keeps_partial_file(void)
{
    reset();
    server.fail_at_read = 3U;
    jetkvm_storage_download_state_t state = run_download();
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_ERROR);
    assert(strcmp(state.error, "The connection to the download server failed.") == 0);
    /* Kept for the next download to continue with a Range request. */
    assert(card.aborts == 1U && card.discards == 0U);
    assert(!card.claimed);
}

static void test_cancel_that_cannot_delete(void)
{
    reset();
    server.cancel_at_read = 2U;
    card.discard_result = ESP_ERR_INVALID_STATE;
    jetkvm_storage_download_state_t state = run_download();
    assert(state.phase == JETKVM_STORAGE_DOWNLOAD_CANCELLED);
    assert(strstr(state.error, "Could not delete the partial os.iso") == state.error);
    assert(card.discards == 1U);
}

int main(void)
{
    assert(!jetkvm_storage_download_cancel());
    test_cancel_during_transfer();
    test_cancel_after_last_byte();
    test_cancel_while_connecting();
    test_cancel_while_connecting_deletes_older_partial();
    test_failure_keeps_partial_file();
    test_cancel_that_cannot_delete();
    puts("storage download task: PASS");
    return 0;
}

/* ---- Fakes ---- */

struct fake_http_client {
    int unused;
};
static struct fake_http_client s_client;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(config->crt_bundle_attach == esp_crt_bundle_attach);
    return &s_client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key,
                                     const char *value)
{
    (void)client; (void)key; (void)value;
    return ESP_OK;
}

esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key)
{
    (void)client; (void)key;
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    (void)client; (void)write_len;
    if (server.cancel_at_open) {
        assert(jetkvm_storage_download_cancel());
    }
    return ESP_OK;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    (void)client;
    return server.status == 200 ? (int64_t)BODY_BYTES : 0;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    (void)client;
    return server.status;
}

int64_t esp_http_client_get_content_length(esp_http_client_handle_t client)
{
    (void)client;
    return server.status == 200 ? (int64_t)BODY_BYTES : 0;
}

esp_err_t esp_http_client_flush_response(esp_http_client_handle_t client, int *len)
{
    (void)client;
    *len = 0;
    return ESP_OK;
}

esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client)
{
    (void)client;
    return ESP_OK;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    (void)client;
    server.reads++;
    if (server.fail_at_read == server.reads) {
        return -1;
    }
    uint64_t left = BODY_BYTES - server.sent;
    size_t n = (size_t)len < READ_BYTES ? (size_t)len : READ_BYTES;
    if (n > left) {
        n = (size_t)left;
    }
    memset(buffer, 'x', n);
    server.sent += n;
    if (server.cancel_at_read == server.reads ||
        (server.cancel_at_last_byte && server.sent == BODY_BYTES)) {
        assert(jetkvm_storage_download_cancel());
    }
    return (int)n;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    (void)client;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    (void)client;
    return ESP_OK;
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    static int64_t now;
    return now += 1000;
}

BaseType_t xTaskCreateWithCaps(TaskFunction_t function, const char *name,
                               uint32_t stack_bytes, void *argument,
                               UBaseType_t priority, TaskHandle_t *task,
                               unsigned caps)
{
    (void)name; (void)stack_bytes; (void)priority; (void)task; (void)caps;
    s_task = function;
    s_task_argument = argument;
    return pdPASS;
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    (void)task;
}

bool jetkvm_memory_policy_wait_reserve_download(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    (void)owner;
    reservation->bytes = 1U;
    return true;
}

bool jetkvm_memory_policy_wait_reserve_psram(
    const char *owner, size_t bytes, jetkvm_memory_policy_reservation_t *reservation)
{
    (void)owner;
    reservation->bytes = bytes;
    return true;
}

void jetkvm_memory_policy_release(jetkvm_memory_policy_reservation_t *reservation)
{
    reservation->bytes = 0U;
}

void *jetkvm_memory_policy_control_alloc(size_t size)
{
    return malloc(size);
}

void jetkvm_memory_policy_control_free(void *memory)
{
    free(memory);
}

esp_err_t jetkvm_virtual_media_get_state(jetkvm_virtual_media_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->card_present = true;
    state->filesystem_mounted = true;
    return ESP_OK;
}

bool jetkvm_virtual_media_upload_claimed(void)
{
    return card.claimed;
}

bool jetkvm_virtual_media_card_in_slot(void)
{
    return true;
}

esp_err_t jetkvm_virtual_media_file_size(const char *filename, uint64_t *size)
{
    if (card.partial && strcmp(filename, "os.iso.incomplete") == 0) {
        *size = 64U * 1024U;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t jetkvm_virtual_media_delete_file(const char *filename)
{
    if (!card.partial || strcmp(filename, "os.iso.incomplete") != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    card.deletes++;
    if (card.delete_result == ESP_OK) {
        card.partial = false;
    }
    return card.delete_result;
}

esp_err_t jetkvm_virtual_media_start_upload(
    const char *filename, uint64_t size, const char *sha256_hex,
    jetkvm_virtual_media_upload_t *upload)
{
    (void)sha256_hex;
    assert(strcmp(filename, "os.iso") == 0 && size == BODY_BYTES);
    card.starts++;
    memset(upload, 0, sizeof(*upload));
    snprintf(upload->upload_id, sizeof(upload->upload_id), "upload_1");
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_claim_upload(
    const char *upload_id, jetkvm_virtual_media_upload_owner_t owner,
    jetkvm_virtual_media_upload_token_t *token,
    uint64_t *uploaded_bytes, uint64_t *expected_bytes)
{
    assert(strcmp(upload_id, "upload_1") == 0);
    assert(owner == JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_DOWNLOAD && !card.claimed);
    card.claims++;
    card.claimed = true;
    *token = 7U;
    if (uploaded_bytes != NULL) {
        *uploaded_bytes = 0U;
    }
    if (expected_bytes != NULL) {
        *expected_bytes = BODY_BYTES;
    }
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_write_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token,
    const void *data, size_t size,
    uint64_t *uploaded_bytes, uint64_t *expected_bytes)
{
    (void)upload_id; (void)data;
    assert(token == 7U && card.claimed);
    card.written += size;
    if (uploaded_bytes != NULL) {
        *uploaded_bytes = card.written;
    }
    if (expected_bytes != NULL) {
        *expected_bytes = BODY_BYTES;
    }
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_finish_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    (void)upload_id;
    assert(token == 7U && card.claimed && card.written == BODY_BYTES);
    card.finishes++;
    card.claimed = false;
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_abort_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    (void)upload_id;
    assert(token == 7U && card.claimed);
    /* The download has failed: a cancel now would not delete this file, so
     * it is refused. */
    assert(!jetkvm_storage_download_cancel());
    card.aborts++;
    card.claimed = false;
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_discard_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    (void)upload_id;
    assert(token == 7U && card.claimed);
    card.discards++;
    /* The session ends also when the partial file stays. */
    card.claimed = false;
    return card.discard_result;
}
