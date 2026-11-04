/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_virtual_media_http.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_upload_gate.h"
#include "http_upload_lifecycle.h"
#include "jetkvm_access.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_virtual_media.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "soc/emac_reg.h"
#include "soc/soc.h"

#define UPLOAD_URI "/storage/upload"
#define UPLOAD_START_URI "/storage/upload/start"
#define STORAGE_FILES_URI "/storage/files"
#define STORAGE_SPACE_URI "/storage/space"
#define STORAGE_MOUNT_URI "/storage/mount"
#define STORAGE_UNMOUNT_URI "/storage/unmount"
#define HTTP_DISCARD_URI "/storage/diagnostics/http-discard"
#define HTTP_DISCARD_CONFIRM_HEADER "X-JetKVM-Diagnostic"
#define HTTP_DISCARD_CONFIRM_VALUE "http-discard-v1"
#define UPLOAD_DMA_ALIGNMENT 128U
#ifndef JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
#define JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT 1
#endif
#ifndef JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE
#define JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE 1
#endif
#ifndef JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB
#define JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB 24
#endif
#ifndef JETKVM_HTTP_UPLOAD_WRITER_PRIORITY
#define JETKVM_HTTP_UPLOAD_WRITER_PRIORITY 6
#endif
#if JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE && \
    !JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
#error "internal upload pipeline requires internal-direct ownership"
#endif
#if JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
/* Selected production path for the known-size, append-only workload.
 * Both hand every byte unchanged to FatFs/SDMMC. The pipelined variant uses
 * one bounded allocation split into two cache-aligned halves so TCP can fill
 * one half while the writer submits the other, without adding a
 * PSRAM-to-internal copy. */
#if JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE
#define UPLOAD_BUFFER_SIZE \
    ((size_t)JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB * 1024U)
#define UPLOAD_BUFFER_COUNT 2U
#define UPLOAD_ALLOCATION_SIZE (UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE)
#define UPLOAD_ALLOCATION_COUNT 1U
#else
#define UPLOAD_BUFFER_SIZE (64U * 1024U)
#define UPLOAD_BUFFER_COUNT 1U
#define UPLOAD_ALLOCATION_SIZE UPLOAD_BUFFER_SIZE
#define UPLOAD_ALLOCATION_COUNT UPLOAD_BUFFER_COUNT
#endif
#define UPLOAD_BUFFER_CAPS \
    (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define UPLOAD_FALLBACK_BUFFER_CAPS \
    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define UPLOAD_BUFFER_SIZE (256U * 1024U)
#define UPLOAD_BUFFER_COUNT 2U
#define UPLOAD_ALLOCATION_SIZE UPLOAD_BUFFER_SIZE
#define UPLOAD_ALLOCATION_COUNT UPLOAD_BUFFER_COUNT
#define UPLOAD_BUFFER_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define UPLOAD_FALLBACK_BUFFER_CAPS UPLOAD_BUFFER_CAPS
#endif
#define UPLOAD_WRITER_STACK_SIZE 6144U
#define UPLOAD_WRITER_PRIORITY \
    (tskIDLE_PRIORITY + JETKVM_HTTP_UPLOAD_WRITER_PRIORITY)
#define UPLOAD_CONTROL_MEMORY_BYTES (3U * 1024U)
/* On the Wi-Fi board an upload fills the link's queue as a download does,
 * with more DMA RAM than the room between the DMA floors. The upload holds
 * the link's reservation until it ends, and its payload takes external-DMA
 * PSRAM so that the queue and the payload never share DMA RAM. The Wi-Fi
 * link, not the TF card, limits the upload's speed there. */
#define UPLOAD_LINK_RESERVE_BYTES JETKVM_MEMORY_DOWNLOAD_LINK_RESERVE_BYTES
#if JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT && CONFIG_ESP_HOSTED_ENABLED
#define UPLOAD_PAYLOAD_PSRAM_FIRST 1
#else
#define UPLOAD_PAYLOAD_PSRAM_FIRST 0
#endif
#define LOCAL_CONTROL_BODY_MAX_BYTES 512U
#define LOCAL_CONTROL_UPLOAD_LEASE_US (60ULL * 1000ULL * 1000ULL)
#define LOCAL_CONTROL_UPLOAD_RETRY_US (5ULL * 1000ULL * 1000ULL)

#if !JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
_Static_assert(UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE ==
                   JETKVM_MEMORY_HTTP_WRITER_POOL_BYTES,
               "HTTP upload writer pool must match the global ingress budget");
#endif
_Static_assert(CONFIG_LWIP_TCP_WND_DEFAULT ==
                   JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES,
               "HTTP TCP receive window must match its memory-policy lease");
_Static_assert(JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE % UPLOAD_DMA_ALIGNMENT == 0U,
               "HTTP upload alignment must divide the storage block size");
_Static_assert(UPLOAD_BUFFER_SIZE % JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE == 0U,
               "HTTP upload buffers must contain complete storage blocks");
_Static_assert(UPLOAD_ALLOCATION_COUNT * UPLOAD_ALLOCATION_SIZE ==
                   UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE,
               "HTTP upload allocations must exactly back every buffer");
#if !JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
_Static_assert(HTTP_DISCARD_BUFFER_SIZE == UPLOAD_BUFFER_SIZE,
               "discard diagnostic must preserve HTTP receive geometry");
#endif

static const char *TAG = "jetkvm_virtual_media_http";

#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
typedef struct {
    portMUX_TYPE lock;
    esp_timer_handle_t timer;
    httpd_handle_t server;
    char upload_id[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY];
} local_control_upload_lease_t;

static local_control_upload_lease_t s_local_control_upload = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
#endif

typedef struct {
    uint8_t *data;
    size_t size;
} upload_chunk_t;

typedef struct {
    uint64_t missed_frames;
    uint64_t fifo_overflow_frames;
    bool missed_counter_overflow;
    bool fifo_counter_overflow;
} emac_rx_drop_counters_t;

typedef struct {
    char upload_id[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY];
    jetkvm_virtual_media_upload_token_t owner_token;
    uint32_t access_epoch;
    QueueHandle_t free_buffers;
    QueueHandle_t ready_chunks;
    SemaphoreHandle_t done;
    esp_err_t result;
    uint32_t write_calls;
    int64_t write_us;
    int64_t max_write_us;
    uint32_t max_write_call;
    uint64_t max_write_emac_missed_frames;
    emac_rx_drop_counters_t emac_during_write;
    emac_rx_drop_counters_t emac_writer_gaps;
    uint64_t max_emac_missed_during_write_frames;
    uint32_t max_emac_missed_write_call;
    int64_t max_emac_missed_write_us;
} upload_pipeline_t;

static emac_rx_drop_counters_t read_and_clear_emac_rx_drop_counters(void)
{
#if JETKVM_BOARD_WIFI
    /* The Wi-Fi board never enables the EMAC; reading its registers faults
     * (load access fault, which rebooted the device at every upload). */
    return (emac_rx_drop_counters_t) {0};
#else
    const uint32_t raw =
        REG_READ(EMAC_MISSEDFRAMEANDBUFFEROVERFLOWCOUNTER_REG);
    return (emac_rx_drop_counters_t) {
        .missed_frames =
            (raw & EMAC_MISFRMCNT_M) >> EMAC_MISFRMCNT_S,
        .fifo_overflow_frames =
            (raw & EMAC_OVFFRMCNT_M) >> EMAC_OVFFRMCNT_S,
        .missed_counter_overflow = (raw & EMAC_MISCNTOVF_M) != 0U,
        .fifo_counter_overflow = (raw & EMAC_OVFCNTOVF_M) != 0U,
    };
#endif
}

static void add_emac_rx_drop_counters(
    emac_rx_drop_counters_t *total,
    const emac_rx_drop_counters_t *sample)
{
    total->missed_frames += sample->missed_frames;
    total->fifo_overflow_frames += sample->fifo_overflow_frames;
    total->missed_counter_overflow |= sample->missed_counter_overflow;
    total->fifo_counter_overflow |= sample->fifo_counter_overflow;
}

static void upload_writer_task(void *argument)
{
    upload_pipeline_t *pipeline = argument;
    upload_chunk_t chunk = {0};
    while (xQueueReceive(pipeline->ready_chunks, &chunk, portMAX_DELAY) ==
           pdTRUE) {
        if (chunk.data == NULL) {
            break;
        }
        if (pipeline->result == ESP_OK) {
            /* The register clears on read. Attribute the idle/queue interval
             * before this call separately, then sample immediately after the
             * sequential storage API call to establish whether RX starvation
             * happened while that call was in flight. Only this task reads
             * between the request-level start and tail samples. */
            const emac_rx_drop_counters_t before_write =
                read_and_clear_emac_rx_drop_counters();
            add_emac_rx_drop_counters(&pipeline->emac_writer_gaps,
                                      &before_write);
            const uint32_t write_call = pipeline->write_calls + 1U;
            int64_t started_us = esp_timer_get_time();
            esp_err_t written = ESP_ERR_INVALID_STATE;
            if (http_upload_gate_write_begin()) {
                written = jetkvm_virtual_media_write_upload_owned(
                    pipeline->upload_id, pipeline->owner_token,
                    chunk.data, chunk.size, NULL, NULL);
                http_upload_gate_write_end();
            }
            __atomic_store_n(&pipeline->result, written, __ATOMIC_RELEASE);
            const int64_t write_us = esp_timer_get_time() - started_us;
            const emac_rx_drop_counters_t during_write =
                read_and_clear_emac_rx_drop_counters();
            add_emac_rx_drop_counters(&pipeline->emac_during_write,
                                      &during_write);
            pipeline->write_us += write_us;
            if (write_us > pipeline->max_write_us) {
                pipeline->max_write_us = write_us;
                pipeline->max_write_call = write_call;
                pipeline->max_write_emac_missed_frames =
                    during_write.missed_frames;
            }
            if (during_write.missed_frames >
                pipeline->max_emac_missed_during_write_frames) {
                pipeline->max_emac_missed_during_write_frames =
                    during_write.missed_frames;
                pipeline->max_emac_missed_write_call = write_call;
                pipeline->max_emac_missed_write_us = write_us;
            }
            pipeline->write_calls = write_call;
        }
        (void)xQueueSend(pipeline->free_buffers, &chunk.data, portMAX_DELAY);
    }
    xSemaphoreGive(pipeline->done);
    vTaskDeleteWithCaps(NULL);
}

static uint64_t megabits_per_second_x100(uint64_t bytes, int64_t elapsed_us)
{
    return elapsed_us > 0 ? bytes * 800ULL / (uint64_t)elapsed_us : 0U;
}

static esp_err_t send_json(httpd_req_t *request, const char *status,
                           const char *body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(request, body);
}

/* Closing a socket that still holds unread request bytes makes lwIP answer
 * with a reset, and the reset can reach the client before the response does:
 * a 4 KiB chunk for a superseded upload got ECONNRESET instead of the 404.
 *
 * So after the response has been sent, a request of up to 256 KiB gets a
 * lingering close, as Go's net/http gives it (Go serves this route
 * upstream): shut down the write side, which follows the response with FIN,
 * then read and discard what the client still sends until it closes or
 * REJECT_LINGER_MAX_US has passed. The reads are nonblocking reads of the
 * socket against one absolute deadline, so the HTTP server task is never
 * held longer, and the bytes are discarded unparsed, so TLS framing on the
 * HTTPS listener does not matter. A larger request, such as a rejected
 * image, is still closed at once with its body unread. */
#define REJECT_LINGER_MAX_BYTES (256U * 1024U)
#define REJECT_LINGER_MAX_US    (500 * 1000)

static void linger_small_request(httpd_req_t *request)
{
    if (request->content_len == 0U ||
        request->content_len > REJECT_LINGER_MAX_BYTES) {
        return;
    }
    const int fd = httpd_req_to_sockfd(request);
    if (fd < 0 || shutdown(fd, SHUT_WR) != 0) {
        return;
    }
    const int64_t deadline = esp_timer_get_time() + REJECT_LINGER_MAX_US;
    char scratch[512];
    for (;;) {
        const int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            return;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        struct timeval wait = {
            .tv_sec = (long)(left / 1000000),
            .tv_usec = (long)(left % 1000000),
        };
        if (select(fd + 1, &readable, NULL, NULL, &wait) <= 0) {
            return;
        }
        /* 0 once the client has closed its side. */
        if (recv(fd, scratch, sizeof(scratch), MSG_DONTWAIT) <= 0) {
            return;
        }
    }
}

/* Returning ESP_OK with an unread request body makes esp_http_server purge
 * the entire remainder before it reuses the connection. A rejected image can
 * be gigabytes long, so close the socket instead of turning an admission
 * failure into a long receive/free loop. The non-ESP_OK return is what tells
 * the server to skip httpd_req_delete(); the header gives the client the same
 * connection-lifetime semantics. */
static esp_err_t send_json_and_close(httpd_req_t *request,
                                     const char *status,
                                     const char *body)
{
    (void)httpd_resp_set_hdr(request, "Connection", "close");
    (void)send_json(request, status, body);
    linger_small_request(request);
    return ESP_FAIL;
}

#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD
static bool http_discard_confirmation_valid(httpd_req_t *request)
{
    const size_t expected_length = strlen(HTTP_DISCARD_CONFIRM_VALUE);
    if (httpd_req_get_hdr_value_len(
            request, HTTP_DISCARD_CONFIRM_HEADER) != expected_length) {
        return false;
    }
    char value[sizeof(HTTP_DISCARD_CONFIRM_VALUE)];
    return httpd_req_get_hdr_value_str(
               request, HTTP_DISCARD_CONFIRM_HEADER, value,
               sizeof(value)) == ESP_OK &&
           strcmp(value, HTTP_DISCARD_CONFIRM_VALUE) == 0;
}

static esp_err_t http_discard_handler(httpd_req_t *request)
{
    if (!http_discard_confirmation_valid(request)) {
        return send_json(
            request, "403 Forbidden",
            "{\"error\":\"Explicit diagnostic confirmation required\"}");
    }
    if (!http_discard_content_length_valid(request->content_len)) {
        return send_json(
            request, "400 Bad Request",
            "{\"error\":\"Diagnostic body must be 16..256 MiB\"}");
    }

    const size_t content_length = (size_t)request->content_len;

    /* Charge the same configured TCP-window plus writer ingress share as a
     * production upload, even though this receive-only probe needs just one
     * physical buffer. The diagnostic must never bypass the active-peer
     * coexistence budget. */
    const jetkvm_memory_policy_request_t memory_request = {
        .psram_bytes = JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES +
                       HTTP_DISCARD_BUFFER_SIZE,
        .psram_largest_block = HTTP_DISCARD_BUFFER_SIZE,
        .storage_ingress_bytes = JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES,
    };
    jetkvm_memory_policy_reservation_t memory_reservation = {0};
    if (!jetkvm_memory_policy_reserve(
            "http-discard-diagnostic", &memory_request,
            &memory_reservation)) {
        return send_json(request, "503 Service Unavailable",
                         "{\"error\":\"Insufficient memory headroom\"}");
    }

    uint8_t *buffer = heap_caps_aligned_alloc(
        UPLOAD_DMA_ALIGNMENT, HTTP_DISCARD_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        jetkvm_memory_policy_release(&memory_reservation);
        return send_json(request, "503 Service Unavailable",
                         "{\"error\":\"Diagnostic buffer unavailable\"}");
    }
    const jetkvm_memory_policy_request_t ingress_lease = {
        .psram_bytes = JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES,
        .psram_largest_block = 2U * 1024U,
        .storage_ingress_bytes = JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES,
    };
    if (!jetkvm_memory_policy_shrink(
            &memory_reservation, &ingress_lease)) {
        heap_caps_free(buffer);
        jetkvm_memory_policy_release(&memory_reservation);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"Failed to retain ingress lease\"}");
    }

    size_t remaining = content_length;
    esp_err_t result = ESP_OK;
    const char *failed_stage = "receive";
    http_discard_receive_metrics_t metrics = http_discard_metrics_init();
    (void)read_and_clear_emac_rx_drop_counters();
    const int64_t started_us = esp_timer_get_time();
    int64_t last_receive_progress_us = started_us;
    while (remaining > 0U) {
        const size_t target = remaining < HTTP_DISCARD_BUFFER_SIZE ?
            remaining : HTTP_DISCARD_BUFFER_SIZE;
        size_t buffered = 0U;
        while (buffered < target) {
            const int64_t receive_started_us = esp_timer_get_time();
            const int received = httpd_req_recv(
                request, (char *)buffer + buffered, target - buffered);
            const int64_t now_us = esp_timer_get_time();
            const int64_t receive_us = now_us - receive_started_us;
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                http_discard_metrics_record(
                    &metrics, 0U, receive_us, true, now_us - started_us);
                if (http_upload_receive_idle_timed_out(
                        now_us, last_receive_progress_us)) {
                    result = ESP_ERR_TIMEOUT;
                    failed_stage = "receive-timeout";
                    break;
                }
                continue;
            }
            http_discard_metrics_record(
                &metrics, received > 0 ? (size_t)received : 0U,
                receive_us, false, now_us - started_us);
            if (received <= 0) {
                result = ESP_FAIL;
                break;
            }
            buffered += (size_t)received;
            remaining -= (size_t)received;
            last_receive_progress_us = now_us;
        }
        if (result != ESP_OK) {
            break;
        }
    }
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    const emac_rx_drop_counters_t emac_drops =
        read_and_clear_emac_rx_drop_counters();
    heap_caps_free(buffer);
    jetkvm_memory_policy_release(&memory_reservation);

    const int64_t non_receive_us = elapsed_us > metrics.receive_us ?
        elapsed_us - metrics.receive_us : 0;
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "HTTP_DISCARD_FAIL stage=%s error=%s bytes=%" PRIu64
                 " remaining=%u total_ms=%lld recv_calls=%" PRIu32
                 " recv_timeouts=%" PRIu32 " emac_missed=%" PRIu64
                 " emac_fifo_overflow=%" PRIu64,
                 failed_stage, esp_err_to_name(result), metrics.bytes,
                 (unsigned)remaining, (long long)(elapsed_us / 1000),
                 metrics.receive_calls, metrics.receive_timeouts,
                 emac_drops.missed_frames, emac_drops.fifo_overflow_frames);
        if (result == ESP_ERR_TIMEOUT) {
            return send_json(request, "408 Request Timeout",
                             "{\"error\":\"Diagnostic receive timed out\"}");
        }
        return send_json(request, "400 Bad Request",
                         "{\"error\":\"Diagnostic body was incomplete\"}");
    }

    const uint64_t mbit_s_x100 =
        megabits_per_second_x100(metrics.bytes, elapsed_us);
    ESP_LOGI(TAG,
             "HTTP_DISCARD_PASS bytes=%" PRIu64
             " total_ms=%lld throughput_mbit_s=%llu.%02llu buffer=%u"
             " recv_calls=%" PRIu32 " recv_timeouts=%" PRIu32
             " recv_ms=%lld non_recv_ms=%lld max_recv_us=%lld"
             " emac_missed=%" PRIu64 " emac_fifo_overflow=%" PRIu64,
             metrics.bytes, (long long)(elapsed_us / 1000),
             (unsigned long long)(mbit_s_x100 / 100U),
             (unsigned long long)(mbit_s_x100 % 100U),
             HTTP_DISCARD_BUFFER_SIZE, metrics.receive_calls,
             metrics.receive_timeouts,
             (long long)(metrics.receive_us / 1000),
             (long long)(non_receive_us / 1000),
             (long long)metrics.max_receive_us, emac_drops.missed_frames,
             emac_drops.fifo_overflow_frames);
    char response[1024];
    snprintf(response, sizeof(response),
             "{\"message\":\"HTTP discard completed\","
             "\"mode\":\"http-discard-v1\",\"bytes\":%" PRIu64 ","
             "\"elapsedMs\":%lld,\"receiveMs\":%lld,"
             "\"nonReceiveMs\":%lld,\"receiveCalls\":%" PRIu32 ","
             "\"receiveTimeouts\":%" PRIu32 ","
             "\"minReceiveBytes\":%" PRIu32 ","
             "\"maxReceiveBytes\":%" PRIu32 ",\"maxReceiveUs\":%lld,"
             "\"firstByteUs\":%lld,\"bufferBytes\":%u,"
             "\"throughputMbitX100\":%llu,"
             "\"emacMissedFrames\":%" PRIu64 ","
             "\"emacFifoOverflowFrames\":%" PRIu64 ","
             "\"emacMissedCounterOverflow\":%s,"
             "\"emacFifoCounterOverflow\":%s}",
             metrics.bytes, (long long)(elapsed_us / 1000),
             (long long)(metrics.receive_us / 1000),
             (long long)(non_receive_us / 1000), metrics.receive_calls,
             metrics.receive_timeouts, metrics.min_receive_bytes,
             metrics.max_receive_bytes, (long long)metrics.max_receive_us,
             (long long)metrics.first_byte_us, HTTP_DISCARD_BUFFER_SIZE,
             (unsigned long long)mbit_s_x100, emac_drops.missed_frames,
             emac_drops.fifo_overflow_frames,
             emac_drops.missed_counter_overflow ? "true" : "false",
             emac_drops.fifo_counter_overflow ? "true" : "false");
    return send_json(request, "200 OK", response);
}
#endif

#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
static esp_err_t send_json_object(httpd_req_t *request, const char *status,
                                  cJSON *root)
{
    char *body = root != NULL ? cJSON_PrintUnformatted(root) : NULL;
    if (body == NULL) {
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"JSON allocation failed\"}");
    }
    esp_err_t result = send_json(request, status, body);
    cJSON_free(body);
    return result;
}

static const char *storage_error_status(esp_err_t error)
{
    switch (error) {
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return "400 Bad Request";
    case ESP_ERR_NOT_FOUND:
        return "404 Not Found";
    case ESP_ERR_INVALID_STATE:
        return "409 Conflict";
    case ESP_ERR_NO_MEM:
        return "503 Service Unavailable";
    default:
        return "500 Internal Server Error";
    }
}

static esp_err_t send_storage_error(httpd_req_t *request, esp_err_t error,
                                    const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        cJSON_AddStringToObject(root, "error", message) == NULL ||
        cJSON_AddNumberToObject(root, "espError", error) == NULL) {
        cJSON_Delete(root);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"JSON allocation failed\"}");
    }
    esp_err_t result = send_json_object(
        request, storage_error_status(error), root);
    cJSON_Delete(root);
    return result;
}

static cJSON *receive_control_json(httpd_req_t *request)
{
    if (request->content_len == 0U ||
        request->content_len > LOCAL_CONTROL_BODY_MAX_BYTES) {
        return NULL;
    }
    char body[LOCAL_CONTROL_BODY_MAX_BYTES + 1U];
    size_t received = 0U;
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (received < request->content_len) {
        if (esp_timer_get_time() >= deadline) return NULL;
        int chunk = httpd_req_recv(
            request, body + received, request->content_len - received);
        if (chunk <= 0) {
            return NULL;
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';
    if (memchr(body, '\0', received) != NULL) {
        return NULL;
    }
    return cJSON_ParseWithLength(body, received);
}

static bool object_has_exact_members(const cJSON *object,
                                     const cJSON *first,
                                     const cJSON *second)
{
    if (!cJSON_IsObject(object) || first == NULL ||
        (second != NULL && first == second)) {
        return false;
    }
    size_t count = 0U;
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        if (item != first && item != second) {
            return false;
        }
        count++;
    }
    return count == (second == NULL ? 1U : 2U);
}

static bool object_has_exactly_three_members(const cJSON *object,
                                             const cJSON *first,
                                             const cJSON *second,
                                             const cJSON *third)
{
    if (!cJSON_IsObject(object) || first == NULL || second == NULL ||
        third == NULL || first == second || first == third || second == third) {
        return false;
    }
    size_t count = 0U;
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        if (item != first && item != second && item != third) {
            return false;
        }
        count++;
    }
    return count == 3U;
}

static bool sha256_hex_valid(const char *value)
{
    if (value == NULL || strlen(value) != 64U) {
        return false;
    }
    for (size_t i = 0U; i < 64U; ++i) {
        char byte = value[i];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f') ||
              (byte >= 'A' && byte <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool local_upload_lease_snapshot(
    char upload_id[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY])
{
    bool present;
    portENTER_CRITICAL(&s_local_control_upload.lock);
    present = s_local_control_upload.upload_id[0] != '\0';
    if (present) {
        memcpy(upload_id, s_local_control_upload.upload_id,
               JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY);
    }
    portEXIT_CRITICAL(&s_local_control_upload.lock);
    return present;
}

static void local_upload_lease_clear_if(const char *upload_id)
{
    portENTER_CRITICAL(&s_local_control_upload.lock);
    if (strcmp(s_local_control_upload.upload_id, upload_id) == 0) {
        s_local_control_upload.upload_id[0] = '\0';
    }
    portEXIT_CRITICAL(&s_local_control_upload.lock);
}

static void local_upload_lease_expire_work(void *argument)
{
    (void)argument;
    char upload_id[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY] = {0};
    if (!local_upload_lease_snapshot(upload_id)) {
        return;
    }
    esp_err_t result = jetkvm_virtual_media_abort_upload(upload_id);
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND) {
        local_upload_lease_clear_if(upload_id);
        ESP_LOGW(TAG, "expired unclaimed local upload id=%s", upload_id);
        return;
    }
    if (result == ESP_ERR_INVALID_STATE) {
        /* A matching HTTP request won the owner claim. It will disarm this
         * marker and perform owner-token cleanup on every terminal path. */
        return;
    }
    ESP_LOGW(TAG, "local upload expiry retry id=%s error=%s",
             upload_id, esp_err_to_name(result));
    if (s_local_control_upload.timer != NULL) {
        (void)esp_timer_start_once(s_local_control_upload.timer,
                                   LOCAL_CONTROL_UPLOAD_RETRY_US);
    }
}

static void local_upload_lease_timeout(void *argument)
{
    (void)argument;
    httpd_handle_t server;
    portENTER_CRITICAL(&s_local_control_upload.lock);
    server = s_local_control_upload.server;
    portEXIT_CRITICAL(&s_local_control_upload.lock);
    if (server == NULL ||
        httpd_queue_work(server, local_upload_lease_expire_work, NULL) !=
            ESP_OK) {
        /* FatFs close may block on SD metadata, so never perform it in the
         * esp_timer task. Retry queueing bounded work while the server lives. */
        if (s_local_control_upload.timer != NULL) {
            (void)esp_timer_start_once(s_local_control_upload.timer,
                                       LOCAL_CONTROL_UPLOAD_RETRY_US);
        }
    }
}

static esp_err_t local_upload_lease_arm(const char *upload_id)
{
    if (s_local_control_upload.timer == NULL || upload_id == NULL ||
        upload_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    (void)esp_timer_stop(s_local_control_upload.timer);
    portENTER_CRITICAL(&s_local_control_upload.lock);
    snprintf(s_local_control_upload.upload_id,
             sizeof(s_local_control_upload.upload_id), "%s", upload_id);
    portEXIT_CRITICAL(&s_local_control_upload.lock);
    esp_err_t result = esp_timer_start_once(
        s_local_control_upload.timer, LOCAL_CONTROL_UPLOAD_LEASE_US);
    if (result != ESP_OK) {
        local_upload_lease_clear_if(upload_id);
    }
    return result;
}

static bool local_upload_lease_take(const char *upload_id)
{
    bool matched = false;
    if (s_local_control_upload.timer != NULL) {
        (void)esp_timer_stop(s_local_control_upload.timer);
    }
    portENTER_CRITICAL(&s_local_control_upload.lock);
    if (upload_id != NULL &&
        strcmp(s_local_control_upload.upload_id, upload_id) == 0) {
        s_local_control_upload.upload_id[0] = '\0';
        matched = true;
    }
    portEXIT_CRITICAL(&s_local_control_upload.lock);
    return matched;
}

static esp_err_t local_upload_lease_abort_current(void)
{
    char upload_id[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY] = {0};
    if (!local_upload_lease_snapshot(upload_id)) {
        return ESP_OK;
    }
    if (s_local_control_upload.timer != NULL) {
        (void)esp_timer_stop(s_local_control_upload.timer);
    }
    esp_err_t result = jetkvm_virtual_media_abort_upload(upload_id);
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND) {
        local_upload_lease_clear_if(upload_id);
        return ESP_OK;
    }
    if (result != ESP_ERR_INVALID_STATE &&
        s_local_control_upload.timer != NULL) {
        (void)esp_timer_start_once(s_local_control_upload.timer,
                                   LOCAL_CONTROL_UPLOAD_RETRY_US);
    }
    return result;
}

static void close_failed_claim(const char *upload_id,
                               jetkvm_virtual_media_upload_token_t owner_token,
                               bool local_control_start)
{
    esp_err_t result = local_control_start ?
        jetkvm_virtual_media_abort_upload_owned(upload_id, owner_token) :
        jetkvm_virtual_media_release_upload(upload_id, owner_token);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "failed to close rejected upload id=%s error=%s",
                 upload_id, esp_err_to_name(result));
    }
}
#endif

static bool allocate_upload_buffers(uint8_t **allocations, bool fallback_to_psram)
{
    for (size_t i = 0; i < UPLOAD_ALLOCATION_COUNT; ++i) {
        allocations[i] = heap_caps_aligned_alloc(
            UPLOAD_DMA_ALIGNMENT, UPLOAD_ALLOCATION_SIZE,
            fallback_to_psram ? UPLOAD_FALLBACK_BUFFER_CAPS :
                                UPLOAD_BUFFER_CAPS);
        if (allocations[i] == NULL) {
            for (size_t j = 0; j < i; ++j) {
                heap_caps_free(allocations[j]);
                allocations[j] = NULL;
            }
            return false;
        }
    }
    return true;
}

static bool allocate_upload_buffers_with_fallback(
    uint8_t **allocations, bool *fallback_to_psram,
    jetkvm_memory_policy_reservation_t *reservation,
    const jetkvm_memory_policy_request_t *fallback_request)
{
    if (allocate_upload_buffers(allocations, *fallback_to_psram)) {
        return true;
    }
    if (*fallback_to_psram || fallback_request == NULL) {
        return false;
    }
    /* Admission is a snapshot, not a physical allocation. Alignment and
     * intervening control allocations can exhaust the preferred block even
     * after admission succeeds. Re-admit the same pipeline in PSRAM before
     * receiving any bytes, just as for a denied Internal/DMA reservation. */
    ESP_LOGW(TAG, "Internal/DMA upload allocation failed; trying external-DMA PSRAM pipeline");
    jetkvm_memory_policy_release(reservation);
    if (!jetkvm_memory_policy_reserve(
            "http-storage-upload-psram", fallback_request, reservation)) {
        return false;
    }
    *fallback_to_psram = true;
    return allocate_upload_buffers(allocations, true);
}

static esp_err_t upload_handler_sync(httpd_req_t *request, uint32_t access_epoch)
{
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0U || query_length > 256U) {
        return send_json_and_close(request, "404 Not Found",
                                   "{\"error\":\"Upload not found\"}");
    }
    char query[257];
    char upload_id[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "uploadId", upload_id,
                              sizeof(upload_id)) != ESP_OK) {
        return send_json_and_close(request, "404 Not Found",
                                   "{\"error\":\"Upload not found\"}");
    }
    jetkvm_virtual_media_upload_token_t owner_token =
        JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID;
    uint64_t uploaded_before_request = 0U;
    esp_err_t claim_result = jetkvm_virtual_media_claim_upload(
        upload_id, JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_HTTP, &owner_token,
        &uploaded_before_request, NULL);
    if (claim_result == ESP_ERR_NOT_FOUND) {
        return send_json_and_close(request, "404 Not Found",
                                   "{\"error\":\"Upload not found\"}");
    }
    if (claim_result == ESP_ERR_INVALID_STATE) {
        return send_json_and_close(
            request, "409 Conflict",
            "{\"error\":\"Upload already has a writer\"}");
    }
    if (claim_result == ESP_ERR_NO_MEM) {
        return send_json_and_close(
            request, "503 Service Unavailable",
            "{\"error\":\"Insufficient DMA memory headroom\"}");
    }
    if (claim_result != ESP_OK) {
        return send_json_and_close(
            request, "500 Internal Server Error",
            "{\"error\":\"Failed to claim upload\"}");
    }

#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
    const bool local_control_start = local_upload_lease_take(upload_id);
#endif

#if JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
    const jetkvm_memory_policy_request_t memory_request = {
        /* The single payload is a real Internal/DMA allocation. Keep charging
         * the full configured HTTP ingress lease so this path cannot
         * create extra coexistence headroom merely by changing buffer media. */
        .internal_bytes = UPLOAD_CONTROL_MEMORY_BYTES +
                          UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE,
        .internal_largest_block = UPLOAD_ALLOCATION_SIZE,
        .dma_bytes = UPLOAD_CONTROL_MEMORY_BYTES +
                     UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE,
        .dma_largest_block = UPLOAD_ALLOCATION_SIZE,
        .psram_bytes = JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES +
                       UPLOAD_WRITER_STACK_SIZE,
        .psram_largest_block = UPLOAD_WRITER_STACK_SIZE,
        .storage_ingress_bytes = JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES,
    };
#else
    const jetkvm_memory_policy_request_t memory_request = {
        /* Two queues, one semaphore and the static-task TCB remain internal
         * even though the writer stack and payloads live in PSRAM. ESP32-P4
         * internal L2 RAM is also DMA-capable, so conservatively charge the
         * same bounded decrease to both overlapping policy domains. */
        .internal_bytes = UPLOAD_CONTROL_MEMORY_BYTES +
                          UPLOAD_LINK_RESERVE_BYTES,
        .internal_largest_block = UPLOAD_CONTROL_MEMORY_BYTES,
        .dma_bytes = UPLOAD_CONTROL_MEMORY_BYTES + UPLOAD_LINK_RESERVE_BYTES,
        .dma_largest_block = UPLOAD_CONTROL_MEMORY_BYTES,
        .psram_bytes = JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES +
                       UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE +
                       UPLOAD_WRITER_STACK_SIZE,
        .psram_largest_block = UPLOAD_BUFFER_SIZE,
        .storage_ingress_bytes =
            JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES,
    };
#endif
    jetkvm_memory_policy_reservation_t memory_reservation = {0};
    bool fallback_to_psram = JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT == 0;
#if JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
    const jetkvm_memory_policy_request_t fallback_memory_request = {
        /* A live 1080p encoder can temporarily consume enough Internal/DMA
         * that the preferred two-half allocation no longer satisfies the
         * global admission floor. ESP32-P4 PSRAM is external-DMA capable, so
         * retain the identical pipeline geometry there instead of rejecting
         * the upload. The VM still validates the actual buffer span before
         * taking its zero-copy SDMMC path and bounces only when required. */
        .internal_bytes = UPLOAD_CONTROL_MEMORY_BYTES +
                          UPLOAD_LINK_RESERVE_BYTES,
        .internal_largest_block = UPLOAD_CONTROL_MEMORY_BYTES,
        .dma_bytes = UPLOAD_CONTROL_MEMORY_BYTES + UPLOAD_LINK_RESERVE_BYTES,
        .dma_largest_block = UPLOAD_CONTROL_MEMORY_BYTES,
        .psram_bytes = JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES +
                       UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE +
                       UPLOAD_WRITER_STACK_SIZE,
        .psram_largest_block = UPLOAD_ALLOCATION_SIZE,
        .storage_ingress_bytes = JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES,
    };
#endif
    bool memory_reserved = !UPLOAD_PAYLOAD_PSRAM_FIRST &&
        jetkvm_memory_policy_reserve(
            "http-storage-upload", &memory_request, &memory_reservation);
#if JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
    if (!memory_reserved) {
        memory_reserved = jetkvm_memory_policy_reserve(
            "http-storage-upload-psram", &fallback_memory_request,
            &memory_reservation);
        fallback_to_psram = memory_reserved;
        if (fallback_to_psram && !UPLOAD_PAYLOAD_PSRAM_FIRST) {
            ESP_LOGW(TAG,
                     "Internal/DMA upload headroom unavailable; using "
                     "external-DMA PSRAM pipeline");
        }
    }
#endif
    if (!memory_reserved) {
        /* Admission has not changed the file. Drop only this request's
         * capability so it can never abort a different producer by ID. */
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
        close_failed_claim(upload_id, owner_token, local_control_start);
#else
        (void)jetkvm_virtual_media_release_upload(upload_id, owner_token);
#endif
        return send_json_and_close(
            request, "503 Service Unavailable",
            "{\"error\":\"Insufficient memory headroom\"}");
    }

    /* The HTTP server commonly receives one TCP segment at a time. Aggregate
     * those fragments into fixed sequential chunks before calling FatFs,
     * otherwise the stream degenerates into thousands of ~1440-byte writes. */
    uint8_t *buffers[UPLOAD_BUFFER_COUNT] = {0};
    uint8_t *allocations[UPLOAD_ALLOCATION_COUNT] = {0};
    upload_pipeline_t pipeline = {
        .owner_token = owner_token,
        .access_epoch = access_epoch,
        .free_buffers = xQueueCreate(UPLOAD_BUFFER_COUNT, sizeof(uint8_t *)),
        .ready_chunks = xQueueCreate(UPLOAD_BUFFER_COUNT,
                                     sizeof(upload_chunk_t)),
        .done = xSemaphoreCreateBinary(),
        .result = ESP_OK,
    };
    snprintf(pipeline.upload_id, sizeof(pipeline.upload_id), "%s", upload_id);
    bool pipeline_ready = pipeline.free_buffers != NULL &&
                          pipeline.ready_chunks != NULL && pipeline.done != NULL;
    const char *pipeline_stage = "queues";
    if (pipeline_ready) {
        pipeline_stage = "payload";
        pipeline_ready = allocate_upload_buffers_with_fallback(
            allocations, &fallback_to_psram, &memory_reservation,
#if JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT
            &fallback_memory_request
#else
            NULL
#endif
        );
    }
    for (size_t i = 0; pipeline_ready && i < UPLOAD_BUFFER_COUNT; ++i) {
        pipeline_stage = "free-buffer queue";
#if JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE
        buffers[i] = allocations[0] + i * UPLOAD_BUFFER_SIZE;
#else
        buffers[i] = allocations[i];
#endif
        pipeline_ready = xQueueSend(
                             pipeline.free_buffers, &buffers[i], 0) == pdTRUE;
    }
    if (pipeline_ready) {
        pipeline_stage = "writer task";
        pipeline_ready = xTaskCreateWithCaps(
            upload_writer_task, "tf_upload_writer", UPLOAD_WRITER_STACK_SIZE,
            &pipeline, UPLOAD_WRITER_PRIORITY, NULL,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    if (!pipeline_ready) {
        ESP_LOGE(TAG, "Failed to start upload pipeline stage=%s domain=%s internal-largest=%u psram-largest=%u",
                 pipeline_stage, fallback_to_psram ? "psram" : "internal",
                 (unsigned)heap_caps_get_largest_free_block(UPLOAD_BUFFER_CAPS),
                 (unsigned)heap_caps_get_largest_free_block(UPLOAD_FALLBACK_BUFFER_CAPS));
        jetkvm_memory_policy_release(&memory_reservation);
        for (size_t i = 0; i < UPLOAD_ALLOCATION_COUNT; ++i) {
            heap_caps_free(allocations[i]);
        }
        if (pipeline.free_buffers != NULL) {
            vQueueDelete(pipeline.free_buffers);
        }
        if (pipeline.ready_chunks != NULL) {
            vQueueDelete(pipeline.ready_chunks);
        }
        if (pipeline.done != NULL) {
            vSemaphoreDelete(pipeline.done);
        }
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
        close_failed_claim(upload_id, owner_token, local_control_start);
#else
        (void)jetkvm_virtual_media_release_upload(upload_id, owner_token);
#endif
        return send_json_and_close(
            request, "500 Internal Server Error",
            "{\"error\":\"Failed to start upload pipeline\"}");
    }
    /* Physical heap use is now visible, but keep the logical ingress share
     * until the writer exits and all payload buffers are freed. */
    const jetkvm_memory_policy_request_t ingress_lease = {
        /* lwIP materializes this advertised window incrementally. Retain
         * physical PSRAM headroom until the request has drained even though
         * the two writer buffers above are already visible to heap_caps. */
        .internal_bytes = UPLOAD_LINK_RESERVE_BYTES,
        .dma_bytes = UPLOAD_LINK_RESERVE_BYTES,
        .psram_bytes = JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES,
        .psram_largest_block = 2U * 1024U,
        .storage_ingress_bytes =
            JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES,
    };
    if (!jetkvm_memory_policy_shrink(
            &memory_reservation, &ingress_lease)) {
        upload_chunk_t stop = {0};
        (void)xQueueSend(pipeline.ready_chunks, &stop, portMAX_DELAY);
        (void)xSemaphoreTake(pipeline.done, portMAX_DELAY);
        jetkvm_memory_policy_release(&memory_reservation);
        for (size_t i = 0; i < UPLOAD_ALLOCATION_COUNT; ++i) {
            heap_caps_free(allocations[i]);
        }
        vQueueDelete(pipeline.free_buffers);
        vQueueDelete(pipeline.ready_chunks);
        vSemaphoreDelete(pipeline.done);
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
        close_failed_claim(upload_id, owner_token, local_control_start);
#else
        (void)jetkvm_virtual_media_release_upload(upload_id, owner_token);
#endif
        return send_json_and_close(
            request, "500 Internal Server Error",
            "{\"error\":\"Failed to retain ingress lease\"}");
    }
    const uint64_t content_length = request->content_len;
    uint64_t remaining = content_length;
    size_t resume_prefix = http_upload_resume_prefix(
        uploaded_before_request, JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE);
    esp_err_t result = ESP_OK;
    const char *failed_stage = "receive";
    uint32_t receive_calls = 0U;
    int64_t receive_us = 0;
    int64_t buffer_wait_us = 0;
    (void)read_and_clear_emac_rx_drop_counters();
    int64_t started_us = esp_timer_get_time();
    int64_t last_receive_progress_us = started_us;
    while (remaining > 0U) {
        uint8_t *buffer = NULL;
        int64_t wait_started_us = esp_timer_get_time();
        if (xQueueReceive(pipeline.free_buffers, &buffer, portMAX_DELAY) !=
            pdTRUE) {
            result = ESP_FAIL;
            failed_stage = "buffer";
            break;
        }
        buffer_wait_us += esp_timer_get_time() - wait_started_us;
        /* A failed write, as after the TF card was removed, ends the
         * request now instead of after the rest of the body. */
        if (__atomic_load_n(&pipeline.result, __ATOMIC_ACQUIRE) != ESP_OK) {
            (void)xQueueSend(pipeline.free_buffers, &buffer, portMAX_DELAY);
            break;
        }
        size_t target = remaining < UPLOAD_BUFFER_SIZE ? (size_t)remaining :
                                                        UPLOAD_BUFFER_SIZE;
        if (resume_prefix != 0U && resume_prefix < target) {
            /* Finish the resumed partial sector in one bounced call. The next
             * chunk then starts at both an aligned buffer base and a sector
             * boundary, so direct I/O can resume instead of bouncing every
             * subsequent buffer due to a permanently shifted source. */
            target = resume_prefix;
        }
        size_t buffered = 0U;
        while (buffered < target) {
            if (http_upload_gate_cancelled() ||
                !jetkvm_access_epoch_valid(pipeline.access_epoch)) {
                result = ESP_ERR_INVALID_STATE;
                failed_stage = "authorization";
                break;
            }
            int64_t receive_started_us = esp_timer_get_time();
            int received = httpd_req_recv(
                request, (char *)buffer + buffered, target - buffered);
            receive_us += esp_timer_get_time() - receive_started_us;
            receive_calls++;
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                if (http_upload_receive_idle_timed_out(
                        esp_timer_get_time(), last_receive_progress_us)) {
                    result = ESP_ERR_TIMEOUT;
                    failed_stage = "receive-timeout";
                    break;
                }
                continue;
            }
            if (received <= 0) {
                result = ESP_FAIL;
                break;
            }
            buffered += (size_t)received;
            remaining -= (size_t)received;
            last_receive_progress_us = esp_timer_get_time();
        }
        if (result != ESP_OK) {
            (void)xQueueSend(pipeline.free_buffers, &buffer, portMAX_DELAY);
            break;
        }
        upload_chunk_t chunk = {.data = buffer, .size = buffered};
        if (xQueueSend(pipeline.ready_chunks, &chunk, portMAX_DELAY) != pdTRUE) {
            result = ESP_FAIL;
            failed_stage = "queue";
            break;
        }
        resume_prefix = 0U;
    }

    upload_chunk_t sentinel = {0};
    (void)xQueueSend(pipeline.ready_chunks, &sentinel, portMAX_DELAY);
    if (xSemaphoreTake(pipeline.done, portMAX_DELAY) != pdTRUE) {
        result = ESP_FAIL;
        failed_stage = "writer";
    } else if (result == ESP_OK && pipeline.result != ESP_OK) {
        result = pipeline.result;
        failed_stage = "write";
    }
    for (size_t i = 0; i < UPLOAD_ALLOCATION_COUNT; ++i) {
        heap_caps_free(allocations[i]);
    }
    vQueueDelete(pipeline.free_buffers);
    vQueueDelete(pipeline.ready_chunks);
    vSemaphoreDelete(pipeline.done);
    jetkvm_memory_policy_release(&memory_reservation);

    int64_t finish_us = 0;
    if (result == ESP_OK && remaining == 0U) {
        failed_stage = "finish";
        int64_t finish_started_us = esp_timer_get_time();
        if (!http_upload_gate_cancelled() &&
            jetkvm_access_lock_epoch(pipeline.access_epoch)) {
            result = jetkvm_virtual_media_finish_upload_owned(upload_id, owner_token);
            jetkvm_access_unlock();
        } else result = ESP_ERR_INVALID_STATE;
        finish_us = esp_timer_get_time() - finish_started_us;
    }
    int64_t elapsed_us = esp_timer_get_time() - started_us;
    const emac_rx_drop_counters_t emac_tail =
        read_and_clear_emac_rx_drop_counters();
    emac_rx_drop_counters_t emac_drops = pipeline.emac_writer_gaps;
    add_emac_rx_drop_counters(&emac_drops, &pipeline.emac_during_write);
    add_emac_rx_drop_counters(&emac_drops, &emac_tail);
    if (result != ESP_OK) {
        /* Keep the .incomplete file for JetKVM-compatible resume, while
         * releasing this request's exclusive upload slot. */
        (void)jetkvm_virtual_media_abort_upload_owned(
            upload_id, owner_token);
        ESP_LOGE(TAG,
                 "UPLOAD_FAIL stage=%s error=%s bytes=%" PRIu64 " remaining=%" PRIu64 " "
                 "total_ms=%lld recv_calls=%" PRIu32 " write_calls=%" PRIu32
                 " emac_missed=%" PRIu64 " emac_fifo_overflow=%" PRIu64,
                 failed_stage, esp_err_to_name(result),
                 content_length - remaining, remaining,
                 (long long)(elapsed_us / 1000), receive_calls,
                 pipeline.write_calls, emac_drops.missed_frames,
                 emac_drops.fifo_overflow_frames);
        const char *status = result == ESP_ERR_TIMEOUT ?
            "408 Request Timeout" : "500 Internal Server Error";
        const char *body = result == ESP_ERR_TIMEOUT ?
            "{\"error\":\"Upload receive timed out\"}" :
            "{\"error\":\"Failed to write upload data\"}";
        if (remaining > 0U) {
            return send_json_and_close(request, status, body);
        }
        return send_json(request, status, body);
    }
    uint64_t mbit_s_x100 =
        megabits_per_second_x100(content_length, elapsed_us);
    ESP_LOGI(TAG,
             "UPLOAD_PASS bytes=%" PRIu64 " total_ms=%lld throughput_mbit_s=%llu.%02llu "
             "buffer=%u buffer_domain=%s recv_calls=%" PRIu32 " recv_ms=%lld "
             "write_calls=%" PRIu32 " write_ms=%lld max_write_us=%lld "
             "wait_ms=%lld finish_ms=%lld emac_missed=%" PRIu64 " "
             "emac_fifo_overflow=%" PRIu64 " emac_missed_in_write=%" PRIu64
             " emac_missed_writer_gaps=%" PRIu64
             " emac_missed_tail=%" PRIu64,
             content_length, (long long)(elapsed_us / 1000),
             (unsigned long long)(mbit_s_x100 / 100U),
             (unsigned long long)(mbit_s_x100 % 100U), UPLOAD_BUFFER_SIZE,
             fallback_to_psram ? "psram-dma" : "internal-dma",
             receive_calls, (long long)(receive_us / 1000),
             pipeline.write_calls,
             (long long)(pipeline.write_us / 1000),
             (long long)pipeline.max_write_us,
             (long long)(buffer_wait_us / 1000),
             (long long)(finish_us / 1000), emac_drops.missed_frames,
             emac_drops.fifo_overflow_frames,
             pipeline.emac_during_write.missed_frames,
             pipeline.emac_writer_gaps.missed_frames,
             emac_tail.missed_frames);
    char response[1024];
    snprintf(response, sizeof(response),
             "{\"message\":\"Upload completed\",\"bytes\":%" PRIu64 ","
             "\"elapsedMs\":%lld,\"receiveMs\":%lld,\"writeMs\":%lld,"
             "\"maxWriteUs\":%lld,\"bufferWaitMs\":%lld,\"finishMs\":%lld,"
             "\"emacMissedFrames\":%" PRIu64 ","
             "\"emacFifoOverflowFrames\":%" PRIu64 ","
             "\"emacMissedCounterOverflow\":%s,"
             "\"emacFifoCounterOverflow\":%s,"
             "\"emacMissedDuringWriteFrames\":%" PRIu64 ","
             "\"emacMissedWriterGapFrames\":%" PRIu64 ","
             "\"emacMissedTailFrames\":%" PRIu64 ","
             "\"maxWriteCall\":%" PRIu32 ","
             "\"maxWriteEmacMissedFrames\":%" PRIu64 ","
             "\"maxEmacMissedWriteCall\":%" PRIu32 ","
             "\"maxEmacMissedDuringWriteFrames\":%" PRIu64 ","
             "\"maxEmacMissedWriteUs\":%lld,"
             "\"receiveCalls\":%" PRIu32 ",\"writeCalls\":%" PRIu32 "}",
             content_length, (long long)(elapsed_us / 1000),
             (long long)(receive_us / 1000),
             (long long)(pipeline.write_us / 1000),
             (long long)pipeline.max_write_us,
             (long long)(buffer_wait_us / 1000),
             (long long)(finish_us / 1000), emac_drops.missed_frames,
             emac_drops.fifo_overflow_frames,
             emac_drops.missed_counter_overflow ? "true" : "false",
             emac_drops.fifo_counter_overflow ? "true" : "false",
             pipeline.emac_during_write.missed_frames,
             pipeline.emac_writer_gaps.missed_frames,
             emac_tail.missed_frames, pipeline.max_write_call,
             pipeline.max_write_emac_missed_frames,
             pipeline.max_emac_missed_write_call,
             pipeline.max_emac_missed_during_write_frames,
             (long long)pipeline.max_emac_missed_write_us,
             receive_calls, pipeline.write_calls);
    return send_json(request, "200 OK", response);
}

typedef struct {
    httpd_req_t *request;
    uint32_t access_epoch;
} async_upload_t;

static void async_upload_worker(void *argument)
{
    async_upload_t *upload = argument;
    int fd = httpd_req_to_sockfd(upload->request);
    httpd_resp_set_hdr(upload->request, "Connection", "close");
    (void)upload_handler_sync(upload->request, upload->access_epoch);
    /* Keep the gate until the descriptor is shut down. Completion wakes
     * HTTPD, which may immediately close/reuse it for another client. */
    (void)shutdown(fd, SHUT_RDWR);
    (void)httpd_req_async_handler_complete(upload->request);
    http_upload_gate_close();
    free(upload);
    vTaskDeleteWithCaps(NULL);
}
static esp_err_t upload_handler(httpd_req_t *request)
{
    async_upload_t *upload = calloc(1, sizeof(*upload));
    if (!upload) return send_json_and_close(request, "500 Internal Server Error", "{\"error\":\"Upload memory unavailable\"}");
    upload->access_epoch = jetkvm_access_request_epoch(request);
    if (!http_upload_gate_open(request->handle, upload->access_epoch,
                               jetkvm_access_epoch_valid)) {
        free(upload);
        return send_json_and_close(request, "409 Conflict", "{\"error\":\"Upload in progress\"}");
    }
    esp_err_t err = httpd_req_async_handler_begin(request, &upload->request);
    if (err == ESP_OK && xTaskCreateWithCaps(async_upload_worker, "http_upload", 8192, upload, 4, NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        (void)httpd_req_async_handler_complete(upload->request);
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        http_upload_gate_close();
        free(upload);
        return send_json_and_close(request, "500 Internal Server Error", "{\"error\":\"Upload worker unavailable\"}");
    }
    return ESP_OK;
}
void jetkvm_virtual_media_http_drain(httpd_handle_t server)
{
    /* Join all request and writer work before HTTPD/TLS can free socket state. */
    while (http_upload_gate_cancel_server(server)) {
        vTaskDelay(1);
    }
}

#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
static esp_err_t start_upload_handler(httpd_req_t *request)
{
    cJSON *root = receive_control_json(request);
    const cJSON *filename = cJSON_IsObject(root) ?
        cJSON_GetObjectItemCaseSensitive(root, "filename") : NULL;
    const cJSON *size = cJSON_IsObject(root) ?
        cJSON_GetObjectItemCaseSensitive(root, "size") : NULL;
    const cJSON *sha256 = cJSON_IsObject(root) ?
        cJSON_GetObjectItemCaseSensitive(root, "sha256") : NULL;
    bool sha256_present = sha256 != NULL;
    bool sha256_valid = !sha256_present ||
        (cJSON_IsString(sha256) && sha256_hex_valid(sha256->valuestring));
    bool members_valid = sha256_present ?
        object_has_exactly_three_members(root, filename, size, sha256) :
        object_has_exact_members(root, filename, size);
    bool valid = cJSON_IsString(filename) && filename->valuestring != NULL &&
        filename->valuestring[0] != '\0' && cJSON_IsNumber(size) &&
        size->valuedouble > 0.0 && size->valuedouble <= (double)UINT32_MAX &&
        size->valuedouble == (double)(uint64_t)size->valuedouble &&
        sha256_valid && members_valid;
    if (!valid) {
        cJSON_Delete(root);
        return send_json(request, "400 Bad Request",
                         "{\"error\":\"filename and uint32 size required; "
                         "sha256 must be 64 hex characters when present\"}");
    }

    char requested_filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    size_t filename_length = strnlen(
        filename->valuestring, sizeof(requested_filename));
    if (filename_length == 0U ||
        filename_length >= sizeof(requested_filename)) {
        cJSON_Delete(root);
        return send_json(request, "400 Bad Request",
                         "{\"error\":\"invalid filename\"}");
    }
    memcpy(requested_filename, filename->valuestring, filename_length + 1U);
    char requested_sha256[
        sizeof(JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE)];
    const char *sha256_value = sha256_present ? sha256->valuestring :
        JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE;
    memcpy(requested_sha256, sha256_value, sizeof(requested_sha256));
    uint64_t requested_size = (uint64_t)size->valuedouble;
    cJSON_Delete(root);

    esp_err_t cleanup = local_upload_lease_abort_current();
    if (cleanup != ESP_OK) {
        return send_storage_error(request, cleanup,
                                  "previous upload is still active");
    }

    jetkvm_virtual_media_upload_t upload = {0};
    esp_err_t result = jetkvm_virtual_media_start_upload(
        requested_filename, requested_size, requested_sha256, &upload);
    if (result != ESP_OK) {
        return send_storage_error(request, result,
                                  "failed to start storage upload");
    }
    result = local_upload_lease_arm(upload.upload_id);
    if (result != ESP_OK) {
        (void)jetkvm_virtual_media_abort_upload(upload.upload_id);
        return send_storage_error(request, result,
                                  "failed to arm upload lease");
    }

    cJSON *response = cJSON_CreateObject();
    bool rendered = response != NULL &&
        cJSON_AddStringToObject(response, "uploadId", upload.upload_id) != NULL &&
        cJSON_AddNumberToObject(response, "alreadyUploadedBytes",
                                (double)upload.already_uploaded_bytes) != NULL &&
        cJSON_AddNumberToObject(response, "size", (double)requested_size) != NULL;
    if (!rendered) {
        cJSON_Delete(response);
        (void)local_upload_lease_take(upload.upload_id);
        (void)jetkvm_virtual_media_abort_upload(upload.upload_id);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"JSON allocation failed\"}");
    }
    result = send_json_object(request, "200 OK", response);
    cJSON_Delete(response);
    if (result != ESP_OK) {
        (void)local_upload_lease_take(upload.upload_id);
        (void)jetkvm_virtual_media_abort_upload(upload.upload_id);
    }
    return result;
}

static esp_err_t list_files_handler(httpd_req_t *request)
{
    jetkvm_virtual_media_file_list_t *files =
        jetkvm_memory_policy_control_alloc(sizeof(*files));
    if (files == NULL) {
        return send_storage_error(request, ESP_ERR_NO_MEM,
                                  "storage file list unavailable");
    }
    esp_err_t result = jetkvm_virtual_media_list_files(files);
    if (result != ESP_OK) {
        jetkvm_memory_policy_control_free(files);
        return send_storage_error(request, result,
                                  "failed to list storage files");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root != NULL ? cJSON_AddArrayToObject(root, "files") : NULL;
    bool rendered = array != NULL;
    for (size_t i = 0U; rendered && i < files->count; ++i) {
        char created_at[32];
        struct tm utc;
        rendered = gmtime_r(&files->files[i].created_at, &utc) != NULL &&
            strftime(created_at, sizeof(created_at),
                     "%Y-%m-%dT%H:%M:%SZ", &utc) > 0U;
        cJSON *entry = rendered ? cJSON_CreateObject() : NULL;
        rendered = entry != NULL &&
            cJSON_AddStringToObject(entry, "filename",
                                   files->files[i].filename) != NULL &&
            cJSON_AddNumberToObject(entry, "size",
                                   (double)files->files[i].size) != NULL &&
            cJSON_AddStringToObject(entry, "createdAt", created_at) != NULL;
        if (rendered) {
            cJSON_AddItemToArray(array, entry);
        } else {
            cJSON_Delete(entry);
        }
    }
    rendered = rendered &&
        cJSON_AddBoolToObject(root, "truncated", files->truncated) != NULL;
    jetkvm_memory_policy_control_free(files);
    if (!rendered) {
        cJSON_Delete(root);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"JSON allocation failed\"}");
    }
    result = send_json_object(request, "200 OK", root);
    cJSON_Delete(root);
    return result;
}

static esp_err_t storage_space_handler(httpd_req_t *request)
{
    jetkvm_virtual_media_space_t space = {0};
    esp_err_t result = jetkvm_virtual_media_get_space(&space);
    if (result != ESP_OK) {
        return send_storage_error(request, result,
                                  "failed to read storage space");
    }
    cJSON *root = cJSON_CreateObject();
    bool rendered = root != NULL &&
        cJSON_AddNumberToObject(root, "bytesUsed", (double)space.bytes_used) != NULL &&
        cJSON_AddNumberToObject(root, "bytesFree", (double)space.bytes_free) != NULL;
    if (!rendered) {
        cJSON_Delete(root);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"JSON allocation failed\"}");
    }
    result = send_json_object(request, "200 OK", root);
    cJSON_Delete(root);
    return result;
}

static esp_err_t delete_file_handler(httpd_req_t *request)
{
    cJSON *root = receive_control_json(request);
    const cJSON *filename = cJSON_IsObject(root) ?
        cJSON_GetObjectItemCaseSensitive(root, "filename") : NULL;
    bool valid = cJSON_IsString(filename) && filename->valuestring != NULL &&
        filename->valuestring[0] != '\0' &&
        object_has_exact_members(root, filename, NULL);
    if (!valid) {
        cJSON_Delete(root);
        return send_json(request, "400 Bad Request",
                         "{\"error\":\"filename required\"}");
    }
    char requested_filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    size_t filename_length = strnlen(
        filename->valuestring, sizeof(requested_filename));
    if (filename_length == 0U ||
        filename_length >= sizeof(requested_filename)) {
        cJSON_Delete(root);
        return send_json(request, "400 Bad Request",
                         "{\"error\":\"invalid filename\"}");
    }
    memcpy(requested_filename, filename->valuestring, filename_length + 1U);
    cJSON_Delete(root);
    esp_err_t result = jetkvm_virtual_media_delete_file(requested_filename);
    return result == ESP_OK ?
        send_json(request, "200 OK", "{\"ok\":true}") :
        send_storage_error(request, result, "failed to delete storage file");
}

static esp_err_t mount_handler(httpd_req_t *request)
{
    cJSON *root = receive_control_json(request);
    const cJSON *filename = cJSON_IsObject(root) ?
        cJSON_GetObjectItemCaseSensitive(root, "filename") : NULL;
    const cJSON *mode = cJSON_IsObject(root) ?
        cJSON_GetObjectItemCaseSensitive(root, "mode") : NULL;
    bool valid = cJSON_IsString(filename) && filename->valuestring != NULL &&
        filename->valuestring[0] != '\0' && cJSON_IsString(mode) &&
        mode->valuestring != NULL && object_has_exact_members(root, filename, mode);
    jetkvm_virtual_media_mode_t requested_mode =
        JETKVM_VIRTUAL_MEDIA_MODE_DISK;
    if (valid && strcmp(mode->valuestring, "CDROM") == 0) {
        requested_mode = JETKVM_VIRTUAL_MEDIA_MODE_CDROM;
    } else if (!valid || strcmp(mode->valuestring, "Disk") != 0) {
        valid = false;
    }
    if (!valid) {
        cJSON_Delete(root);
        return send_json(
            request, "400 Bad Request",
            "{\"error\":\"filename and Disk/CDROM mode required\"}");
    }
    char requested_filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    size_t filename_length = strnlen(
        filename->valuestring, sizeof(requested_filename));
    if (filename_length == 0U ||
        filename_length >= sizeof(requested_filename)) {
        cJSON_Delete(root);
        return send_json(request, "400 Bad Request",
                         "{\"error\":\"invalid filename\"}");
    }
    memcpy(requested_filename, filename->valuestring, filename_length + 1U);
    cJSON_Delete(root);

    esp_err_t pending_result = local_upload_lease_abort_current();
    if (pending_result != ESP_OK) {
        return send_storage_error(request, pending_result,
                                  "failed to abort pending upload");
    }
    esp_err_t result =
        jetkvm_virtual_media_mount(requested_filename, requested_mode);
    return result == ESP_OK ?
        send_json(request, "200 OK", "{\"ok\":true}") :
        send_storage_error(request, result, "failed to mount storage file");
}

static esp_err_t unmount_handler(httpd_req_t *request)
{
    /* A client may have received a start response but failed before opening
     * /storage/upload. Close that ownerless FIL before ordinary cleanup so a
     * following DELETE can never race an open .incomplete path. */
    esp_err_t pending_result = local_upload_lease_abort_current();
    if (pending_result != ESP_OK) {
        return send_storage_error(request, pending_result,
                                  "failed to abort pending upload");
    }
    esp_err_t result = jetkvm_virtual_media_unmount();
    return result == ESP_OK ?
        send_json(request, "200 OK", "{\"ok\":true}") :
        send_storage_error(request, result, "failed to unmount image");
}
#endif

static esp_err_t register_handler(httpd_handle_t server, const char *uri,
                                  httpd_method_t method,
                                  esp_err_t (*handler)(httpd_req_t *))
{
    const httpd_uri_t config = {
        .uri = uri,
        .method = method,
        .handler = handler,
    };
    return jetkvm_access_register(server, &config);
}

esp_err_t jetkvm_virtual_media_http_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = register_handler(
        server, UPLOAD_URI, HTTP_POST, upload_handler);
    if (result != ESP_OK) {
        return result;
    }
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD
    result = register_handler(
        server, HTTP_DISCARD_URI, HTTP_POST, http_discard_handler);
    if (result != ESP_OK) {
        (void)httpd_unregister_uri_handler(server, UPLOAD_URI, HTTP_POST);
        return result;
    }
#endif
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
    const esp_timer_create_args_t timer_config = {
        .callback = local_upload_lease_timeout,
        .name = "local_upload_lease",
    };
    bool created_timer = s_local_control_upload.timer == NULL;
    if (created_timer) result = esp_timer_create(&timer_config, &s_local_control_upload.timer);
    if (result == ESP_OK && created_timer) {
        portENTER_CRITICAL(&s_local_control_upload.lock);
        s_local_control_upload.server = server;
        portEXIT_CRITICAL(&s_local_control_upload.lock);
    }
    if (result == ESP_OK) {
        result = register_handler(server, UPLOAD_START_URI, HTTP_POST,
                                  start_upload_handler);
    }
    if (result == ESP_OK) {
        result = register_handler(server, STORAGE_FILES_URI, HTTP_GET,
                                  list_files_handler);
    }
    if (result == ESP_OK) {
        result = register_handler(server, STORAGE_FILES_URI, HTTP_DELETE,
                                  delete_file_handler);
    }
    if (result == ESP_OK) {
        result = register_handler(server, STORAGE_SPACE_URI, HTTP_GET,
                                  storage_space_handler);
    }
    if (result == ESP_OK) {
        result = register_handler(server, STORAGE_MOUNT_URI, HTTP_POST,
                                  mount_handler);
    }
    if (result == ESP_OK) {
        result = register_handler(server, STORAGE_UNMOUNT_URI, HTTP_POST,
                                  unmount_handler);
    }
    if (result != ESP_OK) {
        (void)httpd_unregister_uri_handler(
            server, STORAGE_UNMOUNT_URI, HTTP_POST);
        (void)httpd_unregister_uri_handler(
            server, STORAGE_MOUNT_URI, HTTP_POST);
        (void)httpd_unregister_uri_handler(
            server, STORAGE_SPACE_URI, HTTP_GET);
        (void)httpd_unregister_uri_handler(
            server, STORAGE_FILES_URI, HTTP_DELETE);
        (void)httpd_unregister_uri_handler(
            server, STORAGE_FILES_URI, HTTP_GET);
        (void)httpd_unregister_uri_handler(
            server, UPLOAD_START_URI, HTTP_POST);
        if (created_timer && s_local_control_upload.timer != NULL) {
            (void)esp_timer_delete(s_local_control_upload.timer);
            s_local_control_upload.timer = NULL;
        }
        portENTER_CRITICAL(&s_local_control_upload.lock);
        if (created_timer) s_local_control_upload.server = NULL;
        portEXIT_CRITICAL(&s_local_control_upload.lock);
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD
        (void)httpd_unregister_uri_handler(
            server, HTTP_DISCARD_URI, HTTP_POST);
#endif
        (void)httpd_unregister_uri_handler(server, UPLOAD_URI, HTTP_POST);
        return result;
    }
#endif
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_http_unregister(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t first = ESP_OK;
    esp_err_t result = ESP_OK;
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
    result = httpd_unregister_uri_handler(
        server, STORAGE_UNMOUNT_URI, HTTP_POST);
    if (first == ESP_OK && result != ESP_OK) {
        first = result;
    }
    result = httpd_unregister_uri_handler(server, STORAGE_MOUNT_URI, HTTP_POST);
    if (first == ESP_OK && result != ESP_OK) {
        first = result;
    }
    result = httpd_unregister_uri_handler(server, STORAGE_SPACE_URI, HTTP_GET);
    if (first == ESP_OK && result != ESP_OK) {
        first = result;
    }
    result = httpd_unregister_uri_handler(
        server, STORAGE_FILES_URI, HTTP_DELETE);
    if (first == ESP_OK && result != ESP_OK) {
        first = result;
    }
    result = httpd_unregister_uri_handler(server, STORAGE_FILES_URI, HTTP_GET);
    if (first == ESP_OK && result != ESP_OK) {
        first = result;
    }
    result = httpd_unregister_uri_handler(server, UPLOAD_START_URI, HTTP_POST);
    if (first == ESP_OK && result != ESP_OK) {
        first = result;
    }
    if (s_local_control_upload.server == server) {
    (void)local_upload_lease_abort_current();
    portENTER_CRITICAL(&s_local_control_upload.lock);
    s_local_control_upload.server = NULL;
    portEXIT_CRITICAL(&s_local_control_upload.lock);
    if (s_local_control_upload.timer != NULL) {
        (void)esp_timer_stop(s_local_control_upload.timer);
        result = esp_timer_delete(s_local_control_upload.timer);
        s_local_control_upload.timer = NULL;
        if (first == ESP_OK && result != ESP_OK) {
            first = result;
        }
    }
    }
#endif
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD
    result = httpd_unregister_uri_handler(
        server, HTTP_DISCARD_URI, HTTP_POST);
    if (first == ESP_OK && result != ESP_OK) {
        first = result;
    }
#endif
    result = httpd_unregister_uri_handler(server, UPLOAD_URI, HTTP_POST);
    return first != ESP_OK ? first : result;
}

void jetkvm_virtual_media_http_revoke_local(void)
{
    /* A password change calls this under the access mutex. The gate waits
     * only for the card write in progress, which takes no access mutex; it
     * never waits for the upload's finish step, which does. */
    esp_err_t result =
        http_upload_gate_revoke(HTTP_UPLOAD_GATE_WRITE_WAIT_US);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "revoked upload write still running after %lld ms",
                 (long long)(HTTP_UPLOAD_GATE_WRITE_WAIT_US / 1000));
    }
#if CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL
    /* An upload that a request has claimed is the gate's; this aborts only
     * an upload that was started and never claimed. */
    (void)local_upload_lease_abort_current();
#endif
}
