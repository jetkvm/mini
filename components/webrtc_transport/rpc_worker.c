#include "rpc_worker.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_access.h"
#include "jetkvm_cloud_registration.h"
#include "jetkvm_factory_reset.h"
#include "jetkvm_usb_service.h"
#include "jetkvm_device_state.h"
#include "jetkvm_memory_policy.h"
#include "peer_sctp_hid_internal.h"
#include "webrtc_rpc_adapters_internal.h"

typedef enum { QUEUE_CONTROL, QUEUE_MEDIA, QUEUE_GUEST, QUEUE_MQTT, QUEUE_UPDATE, QUEUE_COUNT } rpc_queue_t;

/* Methods whose handlers block: HTTP(S) requests, flash writes, a USB
 * detach/reattach through the USB owner (up to its six-second deadline),
 * the card work of an upload's start (the search for a free extent of the
 * file's size and the writes of its identity file and directory entry: tens
 * of milliseconds, and seconds on a FAT volume without such an extent), an
 * OS Services clipboard request or file drop (up to about 2.5 seconds), or
 * an MQTT connection test (up to its five-second timeout, then the client's
 * stop), or an update check (a TLS connection to the update server: up to
 * 2 s of waiting for its memory reservation, the handshake, and up to 4 s
 * for the server's answer). */
static const struct {
    const char *name;
    rpc_queue_t queue;
} s_methods[] = {
    { "factoryReset", QUEUE_CONTROL },
    { "deregisterDevice", QUEUE_CONTROL },
    { "setUsbEmulationState", QUEUE_CONTROL },
    { "checkMountUrl", QUEUE_MEDIA },
    { "mountWithHTTP", QUEUE_MEDIA },
    { "mountWithStorage", QUEUE_MEDIA },
    { "mountBuiltinInstaller", QUEUE_MEDIA },
    { "unmountImage", QUEUE_MEDIA },
    { "formatStorage", QUEUE_MEDIA },
    { "downloadToStorage", QUEUE_MEDIA },
    { "startStorageFileUpload", QUEUE_MEDIA },
    { "setUsbConfig", QUEUE_MEDIA },
    { "setUsbDevices", QUEUE_MEDIA },
    { "setUsbDeviceState", QUEUE_MEDIA },
    { "guestClipboardGet", QUEUE_GUEST },
    { "guestClipboardSet", QUEUE_GUEST },
    { "guestFileDrop", QUEUE_GUEST },
    { "testMqttConnection", QUEUE_MQTT },
    { "getUpdateStatus", QUEUE_UPDATE },
    { "checkUpdateComponents", QUEUE_UPDATE },
};

/* Jobs a queue holds until the peer takes their replies. Media requests run
 * in arrival order, as they did on the peer task: a client may send
 * unmountImage and mountWithStorage together to swap images. */
static const size_t s_capacity[QUEUE_COUNT] = { 1, 4, 2, 1, 1 };

/* The longest request, NUL included, of the control and media methods is
 * mountWithHTTP: its URL plus the JSON-RPC envelope. */
#define RPC_WORKER_INPUT_CAPACITY (JETKVM_VIRTUAL_MEDIA_URL_CAPACITY + 512U)
/* guestClipboardSet carries up to 64 KiB of text, which JSON escaping grows;
 * guestFileDrop's names take at most the 64 KiB of one file.drop. */
#define RPC_WORKER_GUEST_INPUT_CAPACITY (160U * 1024U)
/* The browser may send messages of up to 160 KiB (the SDP's max-message-size). */
_Static_assert(RPC_WORKER_GUEST_INPUT_CAPACITY == 160U * 1024U &&
                   RPC_WORKER_GUEST_INPUT_CAPACITY < PEER_SCTP_RECEIVE_RESERVE,
               "the longest request fits a DataChannel message and the SCTP receive reserve");
#define RPC_WORKER_OUT_OF_MEMORY "out of memory"

typedef struct rpc_job {
    struct rpc_job *next;
    rpc_worker_reply_t reply;
    uint32_t access_epoch;
    bool cloud_authority;
    bool started;
    bool done;
    bool cancelled;
    jetkvm_cloud_config_t config;
    uint32_t config_generation;
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    size_t input_length;
    /* The request and a NUL, in the job's own allocation. */
    char input[];
} rpc_job_t;

/* This lock and the queues outlive transport restarts. Every admitted job
 * runs; a cancelled one cannot reply to a replacement session and keeps its
 * place in the queue until it finishes. One worker task per queue runs the
 * jobs in order. The control worker exits when no job waits, so that the
 * next request gets the stack it needs. The other workers wait up to
 * RPC_WORKER_LINGER_MS for the next job before they exit: a task that
 * deletes itself leaves a cleanup task with an internal-RAM stack and its
 * own control block behind, which only the idle task frees. A stream of
 * OS Services file drops, about 30 a second, with a new worker each, left
 * 17 such tasks (30 to 35 kB of internal RAM) while both cores were busy. */
#define RPC_WORKER_LINGER_MS 1000U
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static rpc_job_t *s_queues[QUEUE_COUNT];
static bool s_worker_running[QUEUE_COUNT];
/* Given when a job joins a queue whose worker runs; a waiting worker takes
 * it. A token left from a job that the worker already took only makes it
 * look at its queue once more. */
static StaticSemaphore_t s_job_added_buffers[QUEUE_COUNT];
static SemaphoreHandle_t s_job_added[QUEUE_COUNT];
/* The last request rpc_worker_defer() saw reached the clipboard's cap. The
 * peer task reads it in its clipboard fallbacks right after a refusal. */
static bool s_over_guest_capacity;

void rpc_worker_release_reply(rpc_worker_reply_t *reply)
{
    jetkvm_memory_policy_control_free(reply->large);
    reply->large = NULL;
}

static void free_job(rpc_job_t *job)
{
    if (job->reply.will_reboot) jetkvm_factory_reset_dispatch_complete();
    if (job->reply.publish_deregistration) {
        jetkvm_device_state_cloud_finish_deregistration();
    }
    rpc_worker_release_reply(&job->reply);
    size_t size = sizeof(*job) + job->input_length + 1U;
    volatile unsigned char *bytes = (volatile unsigned char *)job;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
    jetkvm_memory_policy_control_free(job);
}

static void unlink_locked(rpc_queue_t queue, rpc_job_t *job)
{
    rpc_job_t **link = &s_queues[queue];
    while (*link != job) link = &(*link)->next;
    *link = job->next;
}

/* Marks the next waiting job started. If none waits, a worker that stops
 * marks itself stopped. */
static rpc_job_t *start_next_locked(rpc_queue_t queue, bool stop)
{
    rpc_job_t *job = s_queues[queue];
    while (job && job->started) job = job->next;
    if (job) job->started = true;
    else if (stop) s_worker_running[queue] = false;
    return job;
}

/* The next job that joins the queue within RPC_WORKER_LINGER_MS, or NULL
 * with the worker marked stopped. */
static rpc_job_t *wait_for_job(rpc_queue_t queue)
{
    const TickType_t linger = pdMS_TO_TICKS(RPC_WORKER_LINGER_MS);
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        const TickType_t waited = xTaskGetTickCount() - start;
        const bool woken = waited < linger &&
            xSemaphoreTake(s_job_added[queue], linger - waited) == pdTRUE;
        portENTER_CRITICAL(&s_lock);
        rpc_job_t *job = start_next_locked(queue, !woken);
        portEXIT_CRITICAL(&s_lock);
        if (job || !woken) return job;
    }
}

static bool queue_busy(rpc_queue_t queue)
{
    portENTER_CRITICAL(&s_lock);
    bool busy = s_queues[queue] != NULL;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

static const char *deregister(void *arg)
{
    rpc_job_t *job = arg;
    if (!jetkvm_cloud_config_is_registered(&job->config)) {
        return "Cloud token or URL is not set";
    }
    if (jetkvm_cloud_registration_delete_remote(
            &job->config, job->device_id) != ESP_OK) {
        return "Cloud deregistration request failed";
    }
    if (jetkvm_device_state_cloud_prepare_deregistration(
            job->config_generation, job->config.cloud_url) != ESP_OK) {
        return "Cloud configuration changed or could not be saved";
    }
    job->reply.publish_deregistration = true;
    return NULL;
}

static const char *factory_reset(void *arg)
{
    rpc_job_t *job = arg;
    const char *error = jetkvm_factory_reset_request(NULL);
    job->reply.will_reboot = error == NULL;
    return error;
}

static jetkvm_usb_emulation_change_result_t set_emulation(bool enabled, void *arg)
{
    (void)arg;
    return jetkvm_usb_service_set_enabled(enabled) == JETKVM_USB_SERVICE_CONFIGURATION_COMPLETED
        ? JETKVM_USB_EMULATION_CHANGE_ACCEPTED : JETKVM_USB_EMULATION_CHANGE_UNAVAILABLE;
}

/* Without a reply buffer the clipboard is left alone, and the handler's
 * error, with this reason, fits the inline reply. */
static const char *clipboard_get_out_of_memory(char *text, size_t capacity,
    size_t *length, bool *truncated, const char **status, void *ctx)
{
    (void)text; (void)capacity; (void)length; (void)truncated; (void)status; (void)ctx;
    return RPC_WORKER_OUT_OF_MEMORY;
}

static const char *clipboard_set_out_of_memory(const char *text, size_t length,
                                               const char **status, void *ctx)
{
    (void)text; (void)length; (void)status; (void)ctx;
    return RPC_WORKER_OUT_OF_MEMORY;
}

static const char *file_drop_out_of_memory(const cJSON *files, cJSON *result, void *ctx)
{
    (void)files; (void)result; (void)ctx;
    return RPC_WORKER_OUT_OF_MEMORY;
}

/* Without a reply buffer no request is made, and the error fits the
 * inline reply. */
static bool update_status_out_of_memory(jetkvm_control_ota_status_t *status, void *ctx)
{
    (void)ctx;
    memset(status, 0, sizeof(*status));
    snprintf(status->error, sizeof(status->error), "%s", RPC_WORKER_OUT_OF_MEMORY);
    return true;
}

static bool update_components_out_of_memory(const char *app_constraint,
                                            const char *system_constraint,
                                            bool include_prerelease,
                                            jetkvm_control_ota_status_t *status, void *ctx)
{
    (void)app_constraint; (void)system_constraint; (void)include_prerelease;
    return update_status_out_of_memory(status, ctx);
}

static void run_job(rpc_job_t *job, rpc_queue_t queue)
{
    jetkvm_control_context_t context = {
        .factory_reset = factory_reset,
        .factory_reset_ctx = job,
        .usb_configuration = {
            .set_descriptor_config = rpc_set_usb_descriptor_config,
            .set_devices = rpc_set_usb_devices_adapter,
            .set_device_state = rpc_set_usb_device_state_adapter,
            .set_emulation_enabled = set_emulation,
        },
        .virtual_media = {
            .check_url = rpc_check_virtual_media_url_adapter,
            .mount_http = rpc_mount_virtual_media_http_adapter,
            .mount_storage = rpc_mount_virtual_media_storage_adapter,
            .mount_builtin_installer = rpc_mount_virtual_media_builtin_installer_adapter,
            .unmount = rpc_unmount_virtual_media_adapter,
            .format_storage = rpc_format_virtual_media_storage_adapter,
            .start_upload = rpc_start_virtual_media_upload_adapter,
        },
        .deregister_device = deregister,
        .deregister_device_ctx = job,
        .guest_clipboard = jetkvm_control_default_guest_clipboard_context(),
        .guest_file = jetkvm_control_default_guest_file_context(),
    };
    char *output = job->reply.json;
    size_t capacity = sizeof(job->reply.json);
    if (queue == QUEUE_GUEST) {
        /* A clipboard reply carries up to 64 KiB of text, escaped; a drop's
         * reply, 256 transfer IDs. */
        job->reply.large = jetkvm_memory_policy_control_alloc(JETKVM_MEMORY_RPC_SCRATCH_BYTES);
        if (job->reply.large) {
            output = job->reply.large;
            capacity = JETKVM_MEMORY_RPC_SCRATCH_BYTES;
        } else {
            context.guest_clipboard = (jetkvm_guest_clipboard_context_t){
                .get = clipboard_get_out_of_memory,
                .set = clipboard_set_out_of_memory,
            };
            context.guest_file = (jetkvm_guest_file_context_t){
                .drop = file_drop_out_of_memory,
            };
        }
    } else if (queue == QUEUE_UPDATE) {
        /* The update server's strings are rendered with JSON escaping, which
         * can make the status longer than the inline reply. */
        job->reply.large = jetkvm_memory_policy_control_alloc(JETKVM_MEMORY_RPC_SCRATCH_BYTES);
        if (job->reply.large) {
            output = job->reply.large;
            capacity = JETKVM_MEMORY_RPC_SCRATCH_BYTES;
        } else {
            context.ota = jetkvm_control_default_ota_context();
            context.ota.get_update_status = update_status_out_of_memory;
            context.ota.check_update = update_components_out_of_memory;
        }
    }
    /* A password change between admission and here revokes the request. The
     * mutex is not held across the operation: the peer task takes it for
     * every RPC and must not wait for a USB transaction. */
    bool allowed = job->cloud_authority || jetkvm_access_lock_epoch(job->access_epoch);
    if (allowed && !job->cloud_authority) jetkvm_access_unlock();
    jetkvm_control_result_t result = allowed ? jetkvm_control_handle_rpc_with_context(
        &context, (const uint8_t *)job->input, job->input_length,
        output, capacity, &job->reply.length) :
        JETKVM_CONTROL_NO_RESPONSE;
    if (result != JETKVM_CONTROL_RESPONSE_READY) job->reply.length = 0;
    /* Most guest and update replies are short: they move inline, and the
     * large buffer is given back before the job waits for the peer. */
    if (job->reply.large && job->reply.length < sizeof(job->reply.json)) {
        memcpy(job->reply.json, job->reply.large, job->reply.length);
        job->reply.json[job->reply.length] = '\0';
        rpc_worker_release_reply(&job->reply);
    }
}

static void worker(void *arg)
{
    rpc_queue_t queue = (rpc_queue_t)(uintptr_t)arg;
    const bool lingers = queue != QUEUE_CONTROL;
    portENTER_CRITICAL(&s_lock);
    rpc_job_t *job = start_next_locked(queue, !lingers);
    portEXIT_CRITICAL(&s_lock);
    while (job) {
        run_job(job, queue);
        portENTER_CRITICAL(&s_lock);
        job->done = true;
        bool cancelled = job->cancelled;
        if (cancelled) unlink_locked(queue, job);
        /* The control worker stops here, under the lock, so that the next
         * admission starts a new worker with the stack its request needs. */
        rpc_job_t *next = start_next_locked(queue, !lingers);
        portEXIT_CRITICAL(&s_lock);
        if (cancelled) free_job(job);
        else rpc_worker_notify_done();
        job = next ? next : lingers ? wait_for_job(queue) : NULL;
    }
    vTaskDeleteWithCaps(NULL);
}

/* Only a guest request may be longer than RPC_WORKER_INPUT_CAPACITY. Looking
 * for its name first spares the peer task a second parse of every other long
 * request, such as a 100 KiB saved-macro list. */
static bool may_be_guest_request(const uint8_t *input, size_t length)
{
    static const char clipboard[] = "\"guestClipboard";
    static const char file[] = "\"guestFileDrop\"";
    for (size_t i = 0; i < length; ++i) {
        if (input[i] != '"') continue;
        if (length - i >= sizeof(clipboard) - 1U &&
            memcmp(input + i, clipboard, sizeof(clipboard) - 1U) == 0) return true;
        if (length - i >= sizeof(file) - 1U &&
            memcmp(input + i, file, sizeof(file) - 1U) == 0) return true;
    }
    return false;
}

bool rpc_worker_defer(const uint8_t *input, size_t length,
                      const rpc_worker_origin_t *origin)
{
    s_over_guest_capacity = length >= RPC_WORKER_GUEST_INPUT_CAPACITY;
    if (!origin->generation || s_over_guest_capacity ||
        (length >= RPC_WORKER_INPUT_CAPACITY && !may_be_guest_request(input, length))) {
        return false;
    }
    cJSON *request = cJSON_ParseWithLength((const char *)input, length);
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
    const char *name = NULL;
    rpc_queue_t queue = QUEUE_CONTROL;
    for (size_t i = 0; cJSON_IsString(method) && i < sizeof(s_methods) / sizeof(s_methods[0]); ++i) {
        if (strcmp(method->valuestring, s_methods[i].name) == 0) {
            name = s_methods[i].name;
            queue = s_methods[i].queue;
            break;
        }
    }
    cJSON_Delete(request);
    if (!name || length >= (queue == QUEUE_GUEST ? RPC_WORKER_GUEST_INPUT_CAPACITY :
                            RPC_WORKER_INPUT_CAPACITY)) {
        return false;
    }
    bool reset = strcmp(name, "factoryReset") == 0;
    bool cloud = strcmp(name, "deregisterDevice") == 0;
    rpc_job_t *job = jetkvm_memory_policy_control_alloc(sizeof(*job) + length + 1U);
    if (!job) return false;
    memset(job, 0, sizeof(*job));
    job->input_length = length;
    memcpy(job->input, input, length);
    job->input[length] = '\0';
    if (cloud && (jetkvm_device_state_cloud_snapshot(&job->config,
            &job->config_generation) != ESP_OK ||
        jetkvm_device_state_get_id(job->device_id, sizeof(job->device_id)) != ESP_OK)) {
        free_job(job);
        return false;
    }
    job->access_epoch = origin->access_epoch;
    job->cloud_authority = origin->cloud_authority;
    job->reply.session_id = origin->session_id;
    job->reply.generation = origin->generation;
    job->reply.stream_id = origin->stream_id;
    /* The peer task is the only caller, so the queue's semaphore is made
     * once, before the first worker of its queue can wait on it. */
    if (!s_job_added[queue]) {
        s_job_added[queue] = xSemaphoreCreateBinaryStatic(&s_job_added_buffers[queue]);
    }
    portENTER_CRITICAL(&s_lock);
    size_t queued = 0;
    rpc_job_t **tail = &s_queues[queue];
    for (; *tail; tail = &(*tail)->next) ++queued;
    bool admitted = queued < s_capacity[queue];
    bool start_worker = admitted && !s_worker_running[queue];
    if (admitted) *tail = job;
    if (start_worker) s_worker_running[queue] = true;
    portEXIT_CRITICAL(&s_lock);
    if (!admitted) {
        free_job(job);
        return false;
    }
    if (!start_worker) {
        (void)xSemaphoreGive(s_job_added[queue]);
        return true;
    }
    /* Reset persists its intent directly to NVS, and deregistration clears
     * the cloud configuration there. Flash writes disable cache, so those
     * requests must execute on an internal-RAM stack. The control queue holds
     * one job, so its worker runs only the request it was started for. The
     * media worker's larger stack covers the exFAT format and HTTPS paths the
     * peer task ran; the USB owner, not this task, writes the USB
     * configuration to NVS. The guest worker writes no flash. Its 8 KiB, as
     * the control worker's, covers the dispatcher, cJSON's parse and print
     * (16 levels deep at most) and the wait for the OS Services sender; the
     * request, the clipboard text and the reply are on the heap. */
    unsigned stack_caps = (reset || cloud ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT;
    const char *task_name = reset ? "jk_reset_rpc" : cloud ? "jk_cloud_rpc" :
        queue == QUEUE_MEDIA ? "jk_media_rpc" : queue == QUEUE_GUEST ? "jk_guest_rpc" :
        queue == QUEUE_MQTT ? "jk_mqtt_rpc" : queue == QUEUE_UPDATE ? "jk_update_rpc" :
        "jk_control";
    /* The MQTT test holds the candidate settings three times over (request,
     * native configuration and the client's configuration) and starts and
     * stops an MQTT client: as the media worker, 12 KiB. An update check
     * makes an HTTPS request with its TLS handshake, as the media worker's
     * HTTPS requests do: 12 KiB too. */
    const bool large_stack = queue == QUEUE_MEDIA || queue == QUEUE_MQTT || queue == QUEUE_UPDATE;
    if (xTaskCreateWithCaps(worker, task_name, large_stack ? 12288 : 8192,
                            (void *)(uintptr_t)queue, 4, NULL, stack_caps) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        unlink_locked(queue, job);
        s_worker_running[queue] = false;
        portEXIT_CRITICAL(&s_lock);
        free_job(job);
        return false;
    }
    return true;
}

bool rpc_worker_take_reply(rpc_worker_reply_t *reply)
{
    for (;;) {
        rpc_job_t *job = NULL;
        portENTER_CRITICAL(&s_lock);
        for (size_t queue = 0; queue < QUEUE_COUNT && !job; ++queue) {
            if (s_queues[queue] && s_queues[queue]->done) {
                job = s_queues[queue];
                s_queues[queue] = job->next;
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (!job) return false;
        bool deliver = !job->cancelled && job->reply.length > 0;
        if (deliver) {
            /* The reply's buffer and its follow-up work go to the caller. */
            *reply = job->reply;
            job->reply.large = NULL;
            job->reply.publish_deregistration = false;
            job->reply.will_reboot = false;
        }
        free_job(job);
        if (deliver) return true;
    }
}

void rpc_worker_cancel_replies(void)
{
    for (size_t queue = 0; queue < QUEUE_COUNT; ++queue) {
        for (;;) {
            rpc_job_t *finished = NULL;
            portENTER_CRITICAL(&s_lock);
            for (rpc_job_t *job = s_queues[queue]; job; job = job->next) {
                job->cancelled = true;
                if (job->done && !finished) finished = job;
            }
            if (finished) unlink_locked(queue, finished);
            portEXIT_CRITICAL(&s_lock);
            if (!finished) break;
            free_job(finished);
        }
    }
}

static jetkvm_virtual_media_change_result_t media_not_admitted(void)
{
    return queue_busy(QUEUE_MEDIA) ? JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY
        : JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE;
}

static jetkvm_virtual_media_change_result_t mount_not_admitted(
    const char *source, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{
    (void)source; (void)mode; (void)ctx;
    return media_not_admitted();
}

static jetkvm_virtual_media_change_result_t media_change_not_admitted(void *ctx)
{
    (void)ctx;
    return media_not_admitted();
}

static jetkvm_control_storage_format_result_t format_not_admitted(
    jetkvm_control_storage_space_t *space, void *ctx)
{
    (void)space; (void)ctx;
    return queue_busy(QUEUE_MEDIA) ? JETKVM_CONTROL_STORAGE_FORMAT_BUSY
        : JETKVM_CONTROL_STORAGE_FORMAT_NOT_READY;
}

static jetkvm_virtual_media_change_result_t start_upload_not_admitted(
    const char *filename, uint64_t size, const char *sha256,
    jetkvm_control_storage_upload_t *output, void *ctx)
{
    (void)filename; (void)size; (void)sha256; (void)output; (void)ctx;
    return media_not_admitted();
}

static bool check_url_not_admitted(
    const char *url, jetkvm_control_virtual_media_url_info_t *info, void *ctx)
{
    (void)url; (void)info; (void)ctx;
    return false;
}

static bool download_not_admitted(const char *url, const char *filename,
                                  char *error, size_t capacity, void *ctx)
{
    (void)url; (void)filename; (void)ctx;
    snprintf(error, capacity, "%s", queue_busy(QUEUE_MEDIA) ?
             "another virtual media operation is active" :
             "the download could not start");
    return false;
}

static jetkvm_usb_configuration_change_result_t usb_not_admitted(void)
{
    return queue_busy(QUEUE_MEDIA) ? JETKVM_USB_CONFIGURATION_CHANGE_BUSY
        : JETKVM_USB_CONFIGURATION_CHANGE_UNAVAILABLE;
}

static jetkvm_usb_configuration_change_result_t descriptor_not_admitted(
    const jetkvm_usb_descriptor_config_t *config, void *ctx)
{
    (void)config; (void)ctx;
    return usb_not_admitted();
}

static jetkvm_usb_configuration_change_result_t devices_not_admitted(
    const jetkvm_usb_devices_t *devices, void *ctx)
{
    (void)devices; (void)ctx;
    return usb_not_admitted();
}

static jetkvm_usb_configuration_change_result_t device_state_not_admitted(
    jetkvm_usb_device_t device, bool enabled, void *ctx)
{
    (void)device; (void)enabled; (void)ctx;
    return usb_not_admitted();
}

static jetkvm_usb_emulation_change_result_t emulation_not_admitted(bool enabled, void *ctx)
{
    (void)enabled; (void)ctx;
    return queue_busy(QUEUE_CONTROL) ? JETKVM_USB_EMULATION_CHANGE_BUSY
        : JETKVM_USB_EMULATION_CHANGE_UNAVAILABLE;
}

/* A refused request was too long, found the queue busy, or found no memory
 * for its job or worker. */
static const char *clipboard_not_admitted(void)
{
    return s_over_guest_capacity ? "OS Services clipboard request is too large"
        : queue_busy(QUEUE_GUEST) ? "another OS Services clipboard request is active"
        : "OS Services clipboard request could not start";
}

static const char *clipboard_get_not_admitted(char *text, size_t capacity,
    size_t *length, bool *truncated, const char **status, void *ctx)
{
    (void)text; (void)capacity; (void)length; (void)truncated; (void)status; (void)ctx;
    return clipboard_not_admitted();
}

static const char *clipboard_set_not_admitted(const char *text, size_t length,
                                              const char **status, void *ctx)
{
    (void)text; (void)length; (void)status; (void)ctx;
    return clipboard_not_admitted();
}

static bool mqtt_test_not_admitted(const jetkvm_control_mqtt_settings_t *settings,
                                    jetkvm_control_mqtt_test_result_t *result, void *ctx)
{
    (void)settings; (void)ctx;
    memset(result, 0, sizeof(*result));
    snprintf(result->error, sizeof(result->error), "%s", queue_busy(QUEUE_MQTT) ?
             "another MQTT connection test is active" :
             "the MQTT connection test could not start");
    return true;
}

static bool update_status_not_admitted(jetkvm_control_ota_status_t *status, void *ctx)
{
    (void)ctx;
    memset(status, 0, sizeof(*status));
    snprintf(status->error, sizeof(status->error), "%s", queue_busy(QUEUE_UPDATE) ?
             "another update check is active" : "the update check could not start");
    return true;
}

static bool update_components_not_admitted(const char *app_constraint,
                                           const char *system_constraint,
                                           bool include_prerelease,
                                           jetkvm_control_ota_status_t *status, void *ctx)
{
    (void)app_constraint; (void)system_constraint; (void)include_prerelease;
    return update_status_not_admitted(status, ctx);
}

static const char *file_drop_not_admitted(const cJSON *files, cJSON *result, void *ctx)
{
    (void)files; (void)result; (void)ctx;
    return queue_busy(QUEUE_GUEST) ? "another OS Services request is active"
        : "OS Services file drop could not start";
}

void rpc_worker_peer_fallbacks(jetkvm_control_context_t *context)
{
    context->usb_configuration.set_descriptor_config = descriptor_not_admitted;
    context->usb_configuration.set_devices = devices_not_admitted;
    context->usb_configuration.set_device_state = device_state_not_admitted;
    context->usb_configuration.set_emulation_enabled = emulation_not_admitted;
    context->virtual_media.check_url = check_url_not_admitted;
    context->virtual_media.mount_http = mount_not_admitted;
    context->virtual_media.mount_storage = mount_not_admitted;
    context->virtual_media.mount_builtin_installer = media_change_not_admitted;
    context->virtual_media.unmount = media_change_not_admitted;
    context->virtual_media.format_storage = format_not_admitted;
    context->virtual_media.start_upload = start_upload_not_admitted;
    context->storage_download.start = download_not_admitted;
    context->factory_reset = NULL;
    context->deregister_device = NULL;
    context->guest_clipboard = (jetkvm_guest_clipboard_context_t){
        .get = clipboard_get_not_admitted,
        .set = clipboard_set_not_admitted,
    };
    context->guest_file = (jetkvm_guest_file_context_t){
        .drop = file_drop_not_admitted,
    };
    /* The other MQTT methods return at once and stay on the peer task. */
    context->mqtt = jetkvm_control_default_mqtt_context();
    context->mqtt.test_connection = mqtt_test_not_admitted;
    /* The other update methods return at once and stay on the peer task. */
    context->ota = jetkvm_control_default_ota_context();
    context->ota.get_update_status = update_status_not_admitted;
    context->ota.check_update = update_components_not_admitted;
}
