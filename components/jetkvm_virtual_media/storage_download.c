/* SPDX-License-Identifier: MIT */

#include "jetkvm_storage_download.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "http_range_contract.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_virtual_media_upload_identity.h"
#include "storage_download_contract.h"

#define DOWNLOAD_TIMEOUT_MS 15000
#define DOWNLOAD_REDIRECT_LIMIT 5U
/* Whole 512-byte sectors, cache-line aligned: FatFs hands each one straight
 * to the SDMMC DMA, as it does for the HTTP upload's buffers. */
#define DOWNLOAD_BUFFER_BYTES (64U * 1024U)
#define DOWNLOAD_BUFFER_ALIGNMENT 128U
#define DOWNLOAD_TASK_STACK_BYTES 8192U
#define DOWNLOAD_TASK_PRIORITY 4
#define DOWNLOAD_PROGRESS_INTERVAL_US (500 * 1000)
#define DOWNLOAD_CONTENT_RANGE_CAPACITY 96U
#define DOWNLOAD_INCOMPLETE_SUFFIX ".incomplete"

_Static_assert(DOWNLOAD_BUFFER_BYTES % JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE == 0U,
               "download writes must be whole storage blocks");

static const char *TAG = "storage_download";

typedef struct {
    char *url;
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
} download_request_t;

typedef struct {
    char content_range[DOWNLOAD_CONTENT_RANGE_CAPACITY];
} response_headers_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static jetkvm_storage_download_state_t s_state;
static jetkvm_storage_download_hooks_t s_hooks;
static bool s_running;

void jetkvm_storage_download_set_hooks(
    const jetkvm_storage_download_hooks_t *hooks)
{
    s_hooks = hooks != NULL ? *hooks : (jetkvm_storage_download_hooks_t){0};
}

void jetkvm_storage_download_get_state(jetkvm_storage_download_state_t *state)
{
    if (state == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *state = s_state;
    portEXIT_CRITICAL(&s_lock);
}

static void notify(void)
{
    if (s_hooks.listener != NULL) {
        s_hooks.listener(s_hooks.listener_ctx);
    }
}

static void set_progress(uint64_t bytes, uint64_t total)
{
    portENTER_CRITICAL(&s_lock);
    s_state.bytes = bytes;
    s_state.total = total;
    portEXIT_CRITICAL(&s_lock);
}

static void set_error(const char *format, ...) __attribute__((format(printf, 1, 2)));
static char s_task_error[JETKVM_STORAGE_DOWNLOAD_ERROR_CAPACITY];

static void set_error(const char *format, ...)
{
    if (s_task_error[0] != '\0') {
        return; /* the first failure is the cause */
    }
    va_list args;
    va_start(args, format);
    vsnprintf(s_task_error, sizeof(s_task_error), format, args);
    va_end(args);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_headers_t *headers = event != NULL ? event->user_data : NULL;
    if (headers != NULL && event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != NULL && event->header_value != NULL &&
        strcasecmp(event->header_key, "Content-Range") == 0) {
        snprintf(headers->content_range, sizeof(headers->content_range), "%s",
                 event->header_value);
    }
    return ESP_OK;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

/* GET the object from `offset` on, following redirects, and stop after the
 * response headers. */
static esp_err_t open_response(esp_http_client_handle_t client,
                               response_headers_t *headers, uint64_t offset,
                               int *status, int64_t *content_length)
{
    if (offset > 0U) {
        char range[48];
        snprintf(range, sizeof(range), "bytes=%" PRIu64 "-", offset);
        esp_err_t err = esp_http_client_set_header(client, "Range", range);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        (void)esp_http_client_delete_header(client, "Range");
    }
    for (unsigned redirects = 0U;; ++redirects) {
        headers->content_range[0] = '\0';
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            return err;
        }
        int64_t fetched = esp_http_client_fetch_headers(client);
        *status = esp_http_client_get_status_code(client);
        if (fetched < 0 && *status <= 0) {
            (void)esp_http_client_close(client);
            return ESP_FAIL;
        }
        *content_length = esp_http_client_get_content_length(client);
        if (!is_redirect(*status) || redirects == DOWNLOAD_REDIRECT_LIMIT) {
            return ESP_OK;
        }
        int flushed = 0;
        (void)esp_http_client_flush_response(client, &flushed);
        err = esp_http_client_set_redirection(client);
        (void)esp_http_client_close(client);
        if (err != ESP_OK) {
            return err;
        }
    }
}

static bool check_response(int status, int64_t content_length,
                           const response_headers_t *headers, uint64_t offset,
                           uint64_t *total, uint64_t *skip)
{
    switch (jetkvm_download_check_response(status, content_length,
                                           headers->content_range, offset,
                                           total, skip)) {
    case JETKVM_DOWNLOAD_RESPONSE_OK:
        return true;
    case JETKVM_DOWNLOAD_RESPONSE_HTTP_ERROR:
        set_error("The download server answered HTTP %d.", status);
        return false;
    case JETKVM_DOWNLOAD_RESPONSE_NO_SIZE:
        set_error("The download server did not report the file size.");
        return false;
    case JETKVM_DOWNLOAD_RESPONSE_BAD_RANGE:
    default:
        set_error("The download server did not continue the partial file.");
        return false;
    }
}

/* Read exactly `size` body bytes. */
static bool read_body(esp_http_client_handle_t client, uint8_t *buffer,
                      size_t size)
{
    size_t filled = 0U;
    while (filled < size) {
        int got = esp_http_client_read(client, (char *)buffer + filled,
                                       (int)(size - filled));
        if (got <= 0) {
            set_error("The connection to the download server failed.");
            return false;
        }
        filled += (size_t)got;
    }
    return true;
}

/* Open a write session for `filename`. A file of another size under that name
 * is replaced; a partial file of another object is discarded. */
static bool start_session(const char *filename, uint64_t total,
                          jetkvm_virtual_media_upload_t *upload)
{
    uint64_t existing = 0U;
    esp_err_t stat = jetkvm_virtual_media_file_size(filename, &existing);
    if (stat == ESP_OK &&
        jetkvm_virtual_media_delete_file(filename) != ESP_OK) {
        set_error("Could not replace the older %.64s on the TF card. "
                  "Unmount the virtual media, then try again.", filename);
        return false;
    }
    if (stat != ESP_OK && stat != ESP_ERR_NOT_FOUND) {
        set_error("Could not read the TF card.");
        return false;
    }
    esp_err_t started = jetkvm_virtual_media_start_upload(
        filename, total, JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE,
        upload);
    if ((started == ESP_ERR_INVALID_SIZE || started == ESP_ERR_INVALID_STATE) &&
        !jetkvm_virtual_media_upload_claimed()) {
        char incomplete[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY +
                        sizeof(DOWNLOAD_INCOMPLETE_SUFFIX)];
        snprintf(incomplete, sizeof(incomplete), "%s%s", filename,
                 DOWNLOAD_INCOMPLETE_SUFFIX);
        if (jetkvm_virtual_media_delete_file(incomplete) == ESP_OK) {
            ESP_LOGI(TAG, "discarded a partial %s of another size", filename);
            started = jetkvm_virtual_media_start_upload(
                filename, total, JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE,
                upload);
        }
    }
    if (started == ESP_ERR_NO_MEM) {
        set_error("Not enough free space on the TF card.");
        return false;
    }
    if (started != ESP_OK) {
        set_error("Could not create %.64s on the TF card (%s).", filename,
                  esp_err_to_name(started));
        return false;
    }
    return true;
}

static void download_task(void *argument)
{
    download_request_t *request = argument;
    const bool tls = strncmp(request->url, "https://", 8U) == 0;
    jetkvm_memory_policy_reservation_t memory_ticket = {0};
    jetkvm_memory_policy_reservation_t buffer_ticket = {0};
    response_headers_t headers = {0};
    esp_http_client_handle_t client = NULL;
    jetkvm_virtual_media_upload_t upload = {0};
    jetkvm_virtual_media_upload_token_t token =
        JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID;
    uint8_t *buffer = NULL;
    bool reused = false;
    bool paused = false;
    uint64_t total = 0U;
    uint64_t skip = 0U;
    uint64_t written = 0U;
    int status = 0;
    int64_t content_length = 0;
    uint64_t existing = 0U;
    uint64_t notified_bytes = 0U;
    int64_t notified_us = 0;
    esp_err_t finished = ESP_OK;
    int64_t started_us = esp_timer_get_time();
    s_task_error[0] = '\0';
    notify();

    /* Held for the whole download: the connection's reservation and the
     * Wi-Fi link's blocks, as a firmware download reserves them. */
    if (!jetkvm_memory_policy_wait_reserve_download("storage-download",
                                                    &memory_ticket)) {
        set_error(tls ?
            "Not enough memory for a secure connection. Try again." :
            "Not enough memory to start the download.");
        goto done;
    }
    esp_http_client_config_t config = {
        .url = request->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = DOWNLOAD_TIMEOUT_MS,
        .event_handler = http_event,
        .user_data = &headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    client = esp_http_client_init(&config);
    if (client == NULL) {
        set_error("Not enough memory to start the download.");
        goto done;
    }
    if (open_response(client, &headers, 0U, &status, &content_length) !=
        ESP_OK) {
        set_error("Could not connect to the download server.");
        goto done;
    }
    if (!check_response(status, content_length, &headers, 0U, &total,
                        &skip)) {
        goto done;
    }
    set_progress(0U, total);

    if (jetkvm_virtual_media_file_size(request->filename, &existing) ==
            ESP_OK && existing == total) {
        reused = true;
        written = total;
        goto done;
    }
    if (!start_session(request->filename, total, &upload)) {
        goto done;
    }
    if (jetkvm_virtual_media_claim_upload(
            upload.upload_id, JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_DOWNLOAD,
            &token, &written, NULL) != ESP_OK) {
        set_error("Another upload holds the TF card. Try again.");
        goto done;
    }
    set_progress(written, total);
    if (written > 0U) {
        ESP_LOGI(TAG, "resuming %s at %" PRIu64 " of %" PRIu64 " bytes",
                 request->filename, written, total);
        (void)esp_http_client_close(client);
        if (written < total) {
            uint64_t resumed_total = 0U;
            if (open_response(client, &headers, written, &status,
                              &content_length) != ESP_OK) {
                set_error("Could not connect to the download server.");
                goto done;
            }
            if (!check_response(status, content_length, &headers, written,
                                &resumed_total, &skip)) {
                goto done;
            }
            if (resumed_total != total) {
                set_error("The file on the download server changed. Try again.");
                goto done;
            }
        }
    }

    /* A video rebuild with a PSRAM working set (a debug capture)
     * refuses PSRAM tickets until it ends. */
    if (!jetkvm_memory_policy_wait_reserve_psram(
            "storage-download", DOWNLOAD_BUFFER_BYTES, &buffer_ticket)) {
        set_error("Not enough memory to start the download.");
        goto done;
    }
    buffer = heap_caps_aligned_alloc(DOWNLOAD_BUFFER_ALIGNMENT,
                                     DOWNLOAD_BUFFER_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jetkvm_memory_policy_release(&buffer_ticket);
    if (buffer == NULL) {
        set_error("Not enough memory to start the download.");
        goto done;
    }
    if (written < total && s_hooks.video_pause != NULL) {
        s_hooks.video_pause(true, s_hooks.video_pause_ctx);
        paused = true;
        portENTER_CRITICAL(&s_lock);
        s_state.video_paused = true;
        portEXIT_CRITICAL(&s_lock);
        notify();
    }
    while (skip > 0U) {
        size_t chunk = skip < DOWNLOAD_BUFFER_BYTES ? (size_t)skip :
                                                     DOWNLOAD_BUFFER_BYTES;
        if (!read_body(client, buffer, chunk)) {
            goto done;
        }
        skip -= chunk;
    }
    notified_bytes = written;
    notified_us = esp_timer_get_time();
    while (written < total) {
        size_t chunk = total - written < DOWNLOAD_BUFFER_BYTES ?
                           (size_t)(total - written) : DOWNLOAD_BUFFER_BYTES;
        if (!read_body(client, buffer, chunk)) {
            goto done;
        }
        esp_err_t write_error = jetkvm_virtual_media_write_upload_owned(
            upload.upload_id, token, buffer, chunk, &written, NULL);
        if (write_error != ESP_OK) {
            /* A write to a card that leaves the slot fails before the
             * card-detect task has counted the removal. */
            if (!jetkvm_virtual_media_card_in_slot()) {
                set_error("The TF card was removed during the download.");
            } else {
                set_error("Writing to the TF card failed (%s).",
                          esp_err_to_name(write_error));
            }
            goto done;
        }
        int64_t now_us = esp_timer_get_time();
        if (written < total &&
            jetkvm_download_progress_due(notified_bytes, notified_us, written,
                                         now_us, total,
                                         DOWNLOAD_PROGRESS_INTERVAL_US)) {
            set_progress(written, total);
            notified_bytes = written;
            notified_us = now_us;
            notify();
        }
    }
    finished = jetkvm_virtual_media_finish_upload_owned(upload.upload_id, token);
    if (finished == ESP_OK) {
        token = JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID;
    } else {
        /* A failed close keeps the session and this lease; the abort below
         * retries the close and releases the lease either way. */
        set_error("Could not complete %.64s on the TF card (%s).",
                  request->filename, esp_err_to_name(finished));
    }

done:
    if (token != JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID) {
        /* Keeps the .incomplete prefix for the next attempt to resume. */
        (void)jetkvm_virtual_media_abort_upload_owned(upload.upload_id, token);
    }
    if (client != NULL) {
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    jetkvm_memory_policy_release(&memory_ticket);
    heap_caps_free(buffer);
    if (paused) {
        s_hooks.video_pause(false, s_hooks.video_pause_ctx);
    }
    const bool failed = s_task_error[0] != '\0';
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (failed) {
        ESP_LOGW(TAG, "%s: failed after %" PRId64 " ms at %" PRIu64
                 " bytes: %s", request->filename, elapsed_ms, written,
                 s_task_error);
    } else {
        ESP_LOGI(TAG, "%s: %" PRIu64 " bytes %s in %" PRId64 " ms",
                 request->filename, total,
                 reused ? "already on the card" : "downloaded", elapsed_ms);
    }
    portENTER_CRITICAL(&s_lock);
    s_state.phase = failed ? JETKVM_STORAGE_DOWNLOAD_ERROR :
                             JETKVM_STORAGE_DOWNLOAD_DONE;
    s_state.bytes = written;
    s_state.total = total;
    s_state.reused = reused;
    s_state.video_paused = false;
    memcpy(s_state.error, s_task_error, sizeof(s_state.error));
    s_running = false;
    portEXIT_CRITICAL(&s_lock);
    notify();
    jetkvm_memory_policy_control_free(request->url);
    jetkvm_memory_policy_control_free(request);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t refuse(char *error, size_t capacity, esp_err_t code,
                        const char *reason)
{
    if (error != NULL && capacity > 0U) {
        snprintf(error, capacity, "%s", reason);
    }
    return code;
}

static bool plain_filename(const char *filename)
{
    size_t length = filename != NULL ?
        strnlen(filename, JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY) : 0U;
    return length > 0U && length < JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY &&
           strpbrk(filename, "/\\") == NULL;
}

esp_err_t jetkvm_storage_download_start(const char *url, const char *filename,
                                        char *error, size_t error_capacity)
{
    if (url == NULL || !jetkvm_http_url_is_supported(url) ||
        strnlen(url, JETKVM_VIRTUAL_MEDIA_URL_CAPACITY) >=
            JETKVM_VIRTUAL_MEDIA_URL_CAPACITY) {
        return refuse(error, error_capacity, ESP_ERR_INVALID_ARG,
                      "Enter a valid HTTP or HTTPS URL.");
    }
    if (!plain_filename(filename)) {
        return refuse(error, error_capacity, ESP_ERR_INVALID_ARG,
                      "Enter a file name without a folder.");
    }
    jetkvm_virtual_media_state_t *media =
        jetkvm_memory_policy_control_alloc(sizeof(*media));
    if (media == NULL) {
        return refuse(error, error_capacity, ESP_ERR_NO_MEM,
                      "Not enough memory to start the download.");
    }
    bool card = jetkvm_virtual_media_get_state(media) == ESP_OK &&
                media->card_present && media->filesystem_mounted;
    jetkvm_memory_policy_control_free(media);
    if (!card) {
        return refuse(error, error_capacity, ESP_ERR_NOT_FOUND,
                      "No TF card is mounted. Insert a TF card, then try again.");
    }
    if (jetkvm_virtual_media_upload_claimed()) {
        return refuse(error, error_capacity, ESP_ERR_INVALID_STATE,
                      "A file upload is in progress. Wait for it to finish, "
                      "then try again.");
    }

    size_t url_size = strlen(url) + 1U;
    download_request_t *request =
        jetkvm_memory_policy_control_alloc(sizeof(*request));
    char *url_copy = jetkvm_memory_policy_control_alloc(url_size);
    if (request == NULL || url_copy == NULL) {
        jetkvm_memory_policy_control_free(request);
        jetkvm_memory_policy_control_free(url_copy);
        return refuse(error, error_capacity, ESP_ERR_NO_MEM,
                      "Not enough memory to start the download.");
    }
    memcpy(url_copy, url, url_size);
    request->url = url_copy;
    snprintf(request->filename, sizeof(request->filename), "%s", filename);

    portENTER_CRITICAL(&s_lock);
    bool busy = s_running;
    if (!busy) {
        s_running = true;
        memset(&s_state, 0, sizeof(s_state));
        s_state.phase = JETKVM_STORAGE_DOWNLOAD_DOWNLOADING;
        memcpy(s_state.filename, request->filename, sizeof(s_state.filename));
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        jetkvm_memory_policy_control_free(url_copy);
        jetkvm_memory_policy_control_free(request);
        return refuse(error, error_capacity, ESP_ERR_INVALID_STATE,
                      "Another download is in progress.");
    }
    /* The stack lives in PSRAM, as the HTTP upload worker's does; the body
     * buffer is the only large allocation. */
    if (xTaskCreateWithCaps(download_task, "storage_dl",
                            DOWNLOAD_TASK_STACK_BYTES, request,
                            DOWNLOAD_TASK_PRIORITY, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_running = false;
        s_state.phase = JETKVM_STORAGE_DOWNLOAD_ERROR;
        snprintf(s_state.error, sizeof(s_state.error),
                 "Not enough memory to start the download.");
        portEXIT_CRITICAL(&s_lock);
        jetkvm_memory_policy_control_free(url_copy);
        jetkvm_memory_policy_control_free(request);
        return refuse(error, error_capacity, ESP_ERR_NO_MEM,
                      "Not enough memory to start the download.");
    }
    ESP_LOGI(TAG, "downloading %s to %s", url, filename);
    return ESP_OK;
}
