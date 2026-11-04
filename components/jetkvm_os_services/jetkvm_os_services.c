/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The OS Services task owns the session: it applies USB events, feeds the
 * OUT data to the session, runs its timers and publishes the status STATUS
 * reports. Clipboard requests reach it as commands, and it applies new
 * settings on its next pass; what the browser reads (the video state
 * fields) is a copy the task refreshes.
 */

#include "jetkvm_os_services.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_video.h"
#include "jetkvm_video_quality.h"
#include "nvs.h"
#include "os_services_channels.h"
#include "os_services_edid.h"
#include "os_services_identity.h"
#include "os_services_relay.h"
#include "os_services_session.h"
#include "os_services_settings.h"
#include "os_services_usb.h"
#include "sdkconfig.h"
#include "webrtc_transport.h"

static const char *TAG = "os_services";

#define SETTINGS_NAMESPACE "jk_osvc"
#define SETTINGS_KEY "settings"
#define TASK_STACK_BYTES 8192U
/* Below TinyUSB's task (5), which answers the control requests. */
#define TASK_PRIORITY 4U
#define TASK_CORE 0
#define TICK_MS 100U
/* The OS Services protocol allows the KVM 2 seconds for each write. */
#define WRITE_TIMEOUT_MS 2000U
#define IN_UNREAD_LIMIT_MS 2000
/* Reads handled before the task looks at commands and timers again. */
#define RECEIVE_BUDGET 8U
#define COMMAND_QUEUE_LENGTH 4U
/* The session times a request out after 2 seconds; the caller waits for
 * that answer, and for a write that may take as long. */
#define CLIPBOARD_WAIT_MS (OSVC_CLIPBOARD_TIMEOUT_MS + WRITE_TIMEOUT_MS + 500U)
/* An answer behind held terminal output still reaches the caller. */
_Static_assert(WRITE_TIMEOUT_MS + OSVC_TERMINAL_OUTPUT_HOLD_MS < CLIPBOARD_WAIT_MS,
               "a held terminal.output would outlast a clipboard call");
/* A file drop waits only for its file.drop to be written. */
#define FILE_DROP_WAIT_MS (WRITE_TIMEOUT_MS + 500U)
/* Display names sent with the video state; the videoInputState event must
 * stay within one notification (1280 bytes). The version, with '?' for what
 * JSON would escape, and the names as cJSON prints them take at most
 * VIEW_SENDER_BYTES_MAX. test_os_services_task checks that the longest event,
 * with guestTerminal, guestFile, the longest guestUpdate, the longest source
 * and videoError, still fits. */
#define VIEW_DISPLAYS_MAX 16U
#define VIEW_SENDER_BYTES_MAX 288U
/* How long the transport may take to leave or take the lent buffers: one
 * access unit in flight, as for a capture teardown. */
#define BUFFER_SWITCH_TIMEOUT_MS 3000U
/* Channel buffers this large are admitted by the memory policy first. */
#define CHANNEL_ADMISSION_BYTES 4096U

#define ERROR_UNAVAILABLE "no OS Services clipboard is connected"
#define ERROR_DISABLED \
    "clipboard sharing is turned off in the OS Services settings"

typedef enum {
    COMMAND_CLIPBOARD = 0,
    COMMAND_FILE_DROP,
} command_t;

/* What the browser reads, copied from the session by the task. */
typedef struct {
    bool connected;
    bool approval_pending;
    bool clipboard;
    uint32_t clipboard_flags;
    osvc_target_os_t os;
    bool video_registered;
    osvc_video_source_t source;
    char video_error[OSVC_VIDEO_ERROR_CAPACITY];
    bool terminal;
    bool file_receive;
    bool sender;
    char version[OSVC_INFO_VERSION_MAX + 1U];
    /* NUL-separated. */
    char displays[OSVC_INFO_DISPLAYS_MAX * (OSVC_INFO_DISPLAY_MAX + 1U)];
    size_t display_count;
    char fingerprint[OSVC_FINGERPRINT_TEXT_CAPACITY];
    uint32_t update_generation;
} view_t;

/* One clipboard request between a caller and the task. */
typedef struct {
    SemaphoreHandle_t serial;
    /* Recursive: the task completes a request from inside the session,
     * possibly while it starts another. */
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    bool active;
    bool get;
    const uint8_t *text;
    size_t length;
    char *out;
    size_t capacity;
    uint32_t request_id;
    bool finished;
    const char *error;
    osvc_clipboard_status_t status;
    size_t out_length;
    bool truncated;
} clipboard_call_t;

/* One file drop between a caller and the task. */
typedef struct {
    SemaphoreHandle_t serial;
    /* Held by the task while it writes the drop. */
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    bool active;
    bool finished;
    const osvc_file_entry_t *files;
    size_t count;
    uint32_t *transfer_ids;
    uint32_t drop_id;
    /* The session's reason, copied: it changes with the next drop. */
    char error[128];
} drop_call_t;

static struct {
    /* Set once everything below exists; read from any task. */
    bool started;
    TaskHandle_t task;
    QueueHandle_t commands;
    osvc_session_t *session;
    uint8_t *payload;
    uint32_t seen_reset;
    uint32_t seen_reconfigure;
    bool view_dirty;

    SemaphoreHandle_t settings_lock;
    osvc_bridge_settings_t settings;
    uint32_t settings_sequence;
    /* settings_sequence when the task last applied the settings. */
    uint32_t applied_sequence;
    /* Video was turned off since the task last applied the settings. */
    bool video_turned_off;

    /* Guards the view and s_video's counters, which other tasks read. */
    SemaphoreHandle_t view_lock;
    view_t *view;
    view_t *candidate;

    clipboard_call_t call;
    drop_call_t drop;

    /* The browser's terminal and file channels. The transport's task and
     * this task share the table under channels_lock. */
    SemaphoreHandle_t channels_lock;
    osvc_channels_t *channels;

    portMUX_TYPE listener_lock;
    jetkvm_os_services_listener_t listener;
} s_osvc = {
    .listener_lock = portMUX_INITIALIZER_UNLOCKED,
};

/* The task's service video state, in PSRAM: the OUT read the session has not
 * finished (a frame waits for a slot, and the bus NAKs meanwhile), the slot
 * being filled, the size of the capture buffers the slots borrow (0 while
 * none is lent), and the counters /webrtc/stats reads under view_lock. */
static EXT_RAM_BSS_ATTR struct {
    const uint8_t *held;
    size_t held_length;
    webrtc_transport_au_reservation_t lease;
    size_t lent_bytes;
    uint32_t frame_sequence;
    osvc_video_counters_t stats;
    osvc_video_source_t stats_source;
    osvc_video_rate_t stats_rate;
    uint32_t stats_sender_kbps;
} s_video;

static bool started(void)
{
    return __atomic_load_n(&s_osvc.started, __ATOMIC_ACQUIRE);
}

bool jetkvm_os_services_is_started(void)
{
    return started();
}

static jetkvm_os_services_listener_t current_listener(void)
{
    portENTER_CRITICAL(&s_osvc.listener_lock);
    const jetkvm_os_services_listener_t listener = s_osvc.listener;
    portEXIT_CRITICAL(&s_osvc.listener_lock);
    return listener;
}

void jetkvm_os_services_set_listener(
    const jetkvm_os_services_listener_t *listener)
{
    portENTER_CRITICAL(&s_osvc.listener_lock);
    if (listener == NULL) {
        memset(&s_osvc.listener, 0, sizeof(s_osvc.listener));
    } else {
        s_osvc.listener = *listener;
    }
    portEXIT_CRITICAL(&s_osvc.listener_lock);
}

/* --- Settings ----------------------------------------------------------- */

static void load_settings(osvc_bridge_settings_t *settings)
{
    osvc_bridge_settings_defaults(settings);
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t record[OSVC_SETTINGS_RECORD_LEN];
    size_t length = sizeof(record);
    const esp_err_t error =
        nvs_get_blob(handle, SETTINGS_KEY, record, &length);
    nvs_close(handle);
    if (error == ESP_OK &&
        !osvc_bridge_settings_decode(record, length, settings)) {
        ESP_LOGW(TAG, "OS Services settings record is damaged; using "
                      "defaults");
        osvc_bridge_settings_defaults(settings);
    }
}

static esp_err_t save_settings(const osvc_bridge_settings_t *settings)
{
    uint8_t record[OSVC_SETTINGS_RECORD_LEN];
    osvc_bridge_settings_encode(settings, record);
    nvs_handle_t handle;
    esp_err_t error = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, SETTINGS_KEY, record, sizeof(record));
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static void send_command(command_t command)
{
    if (xQueueSend(s_osvc.commands, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full");
    }
    xTaskNotifyGive(s_osvc.task);
}

bool jetkvm_os_services_get_settings_json(cJSON *object, void *ctx)
{
    (void)ctx;
    if (!started()) {
        return false;
    }
    xSemaphoreTake(s_osvc.settings_lock, portMAX_DELAY);
    const osvc_bridge_settings_t settings = s_osvc.settings;
    xSemaphoreGive(s_osvc.settings_lock);
    return osvc_bridge_settings_to_json(&settings, object);
}

const char *jetkvm_os_services_set_settings_json(const cJSON *request,
                                                 cJSON *applied, void *ctx)
{
    (void)ctx;
    if (!started()) {
        return "OS Services unavailable";
    }
    const char *error = NULL;
    xSemaphoreTake(s_osvc.settings_lock, portMAX_DELAY);
    osvc_bridge_settings_t merged;
    bool changed = false;
    if (osvc_bridge_settings_merge(&s_osvc.settings, request, &merged,
                                   &error)) {
        changed = !osvc_bridge_settings_equal(&merged, &s_osvc.settings);
        if (changed && save_settings(&merged) != ESP_OK) {
            error = "the OS Services settings could not be saved";
            changed = false;
        }
        if (changed) {
            s_osvc.settings = merged;
            s_osvc.settings_sequence++;
            s_osvc.video_turned_off |= !merged.video_enabled;
        }
    }
    const osvc_bridge_settings_t current = s_osvc.settings;
    xSemaphoreGive(s_osvc.settings_lock);
    if (error != NULL) {
        return error;
    }
    if (changed) {
        xTaskNotifyGive(s_osvc.task);
        const jetkvm_os_services_listener_t listener = current_listener();
        if (listener.settings_changed != NULL) {
            listener.settings_changed(listener.ctx);
        }
    }
    if (!osvc_bridge_settings_to_json(&current, applied)) {
        return "out of memory";
    }
    return NULL;
}

bool jetkvm_os_services_render_settings_event(char *output, size_t capacity,
                                              size_t *length)
{
    *length = 0U;
    if (!started()) {
        return false;
    }
    xSemaphoreTake(s_osvc.settings_lock, portMAX_DELAY);
    const osvc_bridge_settings_t settings = s_osvc.settings;
    const uint32_t sequence = s_osvc.settings_sequence;
    xSemaphoreGive(s_osvc.settings_lock);

    cJSON *event = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *object = cJSON_CreateObject();
    bool built = event != NULL && params != NULL && object != NULL &&
                 osvc_bridge_settings_to_json(&settings, object) &&
                 cJSON_AddNumberToObject(params, "sequence", sequence) &&
                 cJSON_AddItemToObject(params, "settings", object);
    if (built) {
        object = NULL;
        built = cJSON_AddStringToObject(event, "jsonrpc", "2.0") &&
                cJSON_AddStringToObject(event, "method",
                                        "osServicesSettings") &&
                cJSON_AddItemToObject(event, "params", params);
        if (built) {
            params = NULL;
        }
    }
    built = built &&
            cJSON_PrintPreallocated(event, output, (int)capacity, false);
    cJSON_Delete(object);
    cJSON_Delete(params);
    cJSON_Delete(event);
    if (built) {
        *length = strlen(output);
    }
    return built;
}

/* --- Video state -------------------------------------------------------- */

static void fill_view(view_t *view)
{
    const osvc_session_t *session = s_osvc.session;
    memset(view, 0, sizeof(*view));
    view->connected = osvc_session_open(session);
    view->approval_pending = session->approval_pending;
    view->clipboard = osvc_session_clipboard_available(session) &&
                      (session->clipboard.flags &
                       (OSVC_CLIPBOARD_FLAG_READ | OSVC_CLIPBOARD_FLAG_WRITE)) !=
                          0U;
    view->clipboard_flags = session->clipboard.flags;
    view->os = session->clipboard.os;
    view->video_registered = view->connected && session->video_registered;
    view->source = session->video_source;
    snprintf(view->video_error, sizeof(view->video_error), "%s",
             session->video_error);
    view->terminal = osvc_session_terminal_available(session);
    view->file_receive = osvc_session_file_available(session);
    view->sender = view->connected && session->sender_info_valid;
    if (view->sender) {
        memcpy(view->version, session->sender_version, sizeof(view->version));
        memcpy(view->displays, session->displays, sizeof(view->displays));
        view->display_count = session->display_count;
    }
    osvc_identity_fingerprint(view->fingerprint);
    view->update_generation = osvc_relay_generation();
}

static void refresh_view(void)
{
    fill_view(s_osvc.candidate);
    xSemaphoreTake(s_osvc.view_lock, portMAX_DELAY);
    const bool changed =
        memcmp(s_osvc.candidate, s_osvc.view, sizeof(view_t)) != 0;
    if (changed) {
        view_t *previous = s_osvc.view;
        s_osvc.view = s_osvc.candidate;
        s_osvc.candidate = previous;
    }
    xSemaphoreGive(s_osvc.view_lock);
    /* Changed or not: a switch that began and ended in one pass leaves the
     * flag that platform_video_stopped() raised. */
    const osvc_video_source_t source = s_osvc.view->source;
    webrtc_transport_set_video_switching(
        source == OSVC_SOURCE_SWITCHING_TO_GUEST ||
        source == OSVC_SOURCE_SWITCHING_TO_PHYSICAL);
    if (changed) {
        const jetkvm_os_services_listener_t listener = current_listener();
        if (listener.state_changed != NULL) {
            listener.state_changed(listener.ctx);
        }
    }
}

/* The bytes cJSON prints for text, with its quotes; a control character
 * counts as the longest escape, \u00XX. */
static size_t json_string_size(const char *text)
{
    size_t size = 2U;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0';
         ++c) {
        size += *c < 0x20U ? 6U : (*c == '"' || *c == '\\') ? 2U : 1U;
    }
    return size;
}

static bool add_sender(cJSON *bridge, const view_t *view)
{
    /* What JSON escapes would grow in the version is shown as '?'. */
    char version[sizeof(view->version)];
    for (size_t i = 0U; i < sizeof(version); ++i) {
        const unsigned char c = (unsigned char)view->version[i];
        version[i] = c != '\0' && (c < 0x20U || c == '"' || c == '\\') ?
                         '?' : (char)c;
    }
    cJSON *sender = cJSON_AddObjectToObject(bridge, "sender");
    cJSON *displays = NULL;
    if (sender == NULL ||
        !cJSON_AddStringToObject(sender, "version", version) ||
        (displays = cJSON_AddArrayToObject(sender, "displays")) == NULL) {
        return false;
    }
    const char *name = view->displays;
    size_t bytes = json_string_size(version);
    for (size_t i = 0U; i < view->display_count && i < VIEW_DISPLAYS_MAX;
         ++i) {
        const size_t length = strlen(name);
        bytes += json_string_size(name) + 1U; /* and a comma */
        if (bytes > VIEW_SENDER_BYTES_MAX) {
            break;
        }
        cJSON *item = cJSON_CreateString(name);
        if (item == NULL || !cJSON_AddItemToArray(displays, item)) {
            cJSON_Delete(item);
            return false;
        }
        name += length + 1U;
    }
    return true;
}

bool jetkvm_os_services_add_video_state(cJSON *object, void *ctx)
{
    (void)ctx;
    if (!started()) {
        return true;
    }
    xSemaphoreTake(s_osvc.view_lock, portMAX_DELAY);
    const view_t *view = s_osvc.view;
    cJSON *bridge = NULL;
    bool ok = cJSON_AddStringToObject(object, "source",
                                      osvc_video_source_name(view->source)) !=
                  NULL &&
              (bridge = cJSON_AddObjectToObject(object, "osServices")) != NULL &&
              (!view->sender || add_sender(bridge, view)) &&
              cJSON_AddBoolToObject(bridge, "connected", view->connected) &&
              cJSON_AddBoolToObject(bridge, "video",
                                    view->source == OSVC_SOURCE_GUEST) &&
              cJSON_AddBoolToObject(bridge, "videoRegistered",
                                    view->video_registered) &&
              (view->video_error[0] == '\0' ||
               cJSON_AddStringToObject(bridge, "videoError",
                                       view->video_error) != NULL) &&
              cJSON_AddBoolToObject(bridge, "clipboard", view->clipboard) &&
              cJSON_AddBoolToObject(bridge, "approvalPending",
                                    view->approval_pending) &&
              cJSON_AddStringToObject(bridge, "fingerprint",
                                      view->fingerprint);
    if (ok && view->clipboard) {
        cJSON *clipboard = cJSON_AddObjectToObject(object, "guestClipboard");
        ok = clipboard != NULL &&
             cJSON_AddBoolToObject(clipboard, "read",
                                   (view->clipboard_flags &
                                    OSVC_CLIPBOARD_FLAG_READ) != 0U) &&
             cJSON_AddBoolToObject(clipboard, "write",
                                   (view->clipboard_flags &
                                    OSVC_CLIPBOARD_FLAG_WRITE) != 0U) &&
             cJSON_AddStringToObject(clipboard, "os",
                                     osvc_target_os_name(view->os));
    }
    /* Left out when false, as the browser reads a missing field. */
    if (ok && view->terminal) {
        ok = cJSON_AddBoolToObject(object, "guestTerminal", true) != NULL;
    }
    if (ok && view->file_receive) {
        cJSON *file = cJSON_AddObjectToObject(object, "guestFile");
        ok = file != NULL && cJSON_AddBoolToObject(file, "receive", true);
    }
    xSemaphoreGive(s_osvc.view_lock);
    return ok && osvc_relay_add_state(object);
}

const char *jetkvm_os_services_update_start(void *ctx)
{
    (void)ctx;
    return osvc_relay_start();
}

/* --- Clipboard ---------------------------------------------------------- */

/* Under call.lock, from the task. */
static void finish_call(const char *error, osvc_clipboard_status_t status)
{
    clipboard_call_t *call = &s_osvc.call;
    call->finished = true;
    call->error = error;
    call->status = status;
    xSemaphoreGive(call->done);
}

static void start_clipboard_request(void)
{
    clipboard_call_t *call = &s_osvc.call;
    xSemaphoreTakeRecursive(call->lock, portMAX_DELAY);
    if (call->active && !call->finished && call->request_id == 0U) {
        osvc_session_t *session = s_osvc.session;
        osvc_clipboard_status_t status;
        const uint32_t id = osvc_session_clipboard_request(
            session, call->get ? NULL : call->text, call->length, &status);
        if (id != 0U) {
            call->request_id = id;
        } else if (!session->settings.clipboard_enabled) {
            finish_call(ERROR_DISABLED, status);
        } else if (status == OSVC_CLIPBOARD_UNSUPPORTED) {
            finish_call(ERROR_UNAVAILABLE, status);
        } else {
            finish_call("the clipboard request could not be sent", status);
        }
    }
    xSemaphoreGiveRecursive(call->lock);
}

static void clipboard_done(void *ctx, uint32_t request_id,
                           osvc_clipboard_status_t status, bool answered,
                           const uint8_t *text, size_t length, bool truncated)
{
    (void)ctx;
    clipboard_call_t *call = &s_osvc.call;
    xSemaphoreTakeRecursive(call->lock, portMAX_DELAY);
    if (call->active && !call->finished && call->request_id == request_id) {
        if (!answered) {
            finish_call(osvc_session_open(s_osvc.session) ?
                            "the target computer's clipboard did not answer in time" :
                            ERROR_UNAVAILABLE,
                        status);
        } else {
            if (call->get && status == OSVC_CLIPBOARD_OK && text != NULL) {
                const size_t kept =
                    length < call->capacity ? length : call->capacity;
                memcpy(call->out, text, kept);
                call->out_length = kept;
                call->truncated = truncated || kept < length;
            } else {
                call->truncated = truncated;
            }
            finish_call(NULL, status);
        }
    }
    xSemaphoreGiveRecursive(call->lock);
}

static const char *clipboard_call(bool get, const uint8_t *text,
                                  size_t length, char *out, size_t capacity,
                                  size_t *out_length, bool *truncated,
                                  const char **status)
{
    *status = "failed";
    if (!started()) {
        return ERROR_UNAVAILABLE;
    }
    clipboard_call_t *call = &s_osvc.call;
    if (xSemaphoreTake(call->serial, pdMS_TO_TICKS(CLIPBOARD_WAIT_MS)) !=
        pdTRUE) {
        return "the OS Services clipboard is busy";
    }
    xSemaphoreTakeRecursive(call->lock, portMAX_DELAY);
    call->active = true;
    call->get = get;
    call->text = text;
    call->length = length;
    call->out = out;
    call->capacity = capacity;
    call->request_id = 0U;
    call->finished = false;
    call->error = NULL;
    call->status = OSVC_CLIPBOARD_FAILED;
    call->out_length = 0U;
    call->truncated = false;
    (void)xSemaphoreTake(call->done, 0);
    xSemaphoreGiveRecursive(call->lock);

    send_command(COMMAND_CLIPBOARD);
    const bool answered =
        xSemaphoreTake(call->done, pdMS_TO_TICKS(CLIPBOARD_WAIT_MS)) == pdTRUE;

    /* After this the task no longer touches text or out. */
    xSemaphoreTakeRecursive(call->lock, portMAX_DELAY);
    call->active = false;
    (void)answered;
    const char *error = call->finished ?
                            call->error :
                            "the target computer's clipboard did not answer in time";
    if (error == NULL) {
        *status = osvc_clipboard_status_name(call->status);
        if (out_length != NULL) {
            *out_length = call->out_length;
        }
        if (truncated != NULL) {
            *truncated = call->truncated;
        }
    }
    xSemaphoreGiveRecursive(call->lock);
    xSemaphoreGive(call->serial);
    return error;
}

const char *jetkvm_os_services_clipboard_get(char *text, size_t capacity,
                                             size_t *length, bool *truncated,
                                             const char **status, void *ctx)
{
    (void)ctx;
    *length = 0U;
    *truncated = false;
    return clipboard_call(true, NULL, 0U, text, capacity, length, truncated,
                          status);
}

const char *jetkvm_os_services_clipboard_set(const char *text, size_t length,
                                             const char **status, void *ctx)
{
    (void)ctx;
    return clipboard_call(false, (const uint8_t *)text, length, NULL, 0U,
                          NULL, NULL, status);
}

/* --- File drops --------------------------------------------------------- */

/* From the task: writes the drop the caller is waiting for. */
static void start_file_drop(void)
{
    drop_call_t *call = &s_osvc.drop;
    xSemaphoreTake(call->lock, portMAX_DELAY);
    if (call->active && !call->finished) {
        uint32_t drop_id = 0U;
        const char *error = osvc_session_file_drop(
            s_osvc.session, call->files, call->count, &drop_id,
            call->transfer_ids);
        call->drop_id = drop_id;
        if (error != NULL) {
            snprintf(call->error, sizeof(call->error), "%s", error);
        }
        call->finished = true;
        xSemaphoreGive(call->done);
    }
    xSemaphoreGive(call->lock);
}

/* The entries of files, which the caller checked; NULL with *error set when
 * they cannot be held. */
static osvc_file_entry_t *file_entries(const cJSON *files, size_t count,
                                       const char **error)
{
    /* An empty drop still gets an entry, for the session to refuse. */
    osvc_file_entry_t *entries = heap_caps_calloc(
        count > 0U ? count : 1U, sizeof(*entries), MALLOC_CAP_SPIRAM);
    if (entries == NULL) {
        *error = "out of memory";
        return NULL;
    }
    size_t i = 0U;
    const cJSON *file = NULL;
    cJSON_ArrayForEach(file, files) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(file, "name");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size");
        if (i == count || !cJSON_IsString(name) || name->valuestring == NULL ||
            !cJSON_IsNumber(size) || size->valuedouble < 0.0 ||
            size->valuedouble > 9007199254740992.0) {
            heap_caps_free(entries);
            *error = "invalid file entry";
            return NULL;
        }
        entries[i].name = name->valuestring;
        entries[i].name_length = strlen(name->valuestring);
        entries[i].size = (uint64_t)size->valuedouble;
        ++i;
    }
    return entries;
}

static bool add_drop_result(cJSON *result, uint32_t drop_id,
                            const uint32_t *transfer_ids, size_t count)
{
    char channel[sizeof(OSVC_FILE_CHANNEL_PREFIX) + 10U];
    snprintf(channel, sizeof(channel), "%s%" PRIu32, OSVC_FILE_CHANNEL_PREFIX,
             drop_id);
    cJSON *transfers = NULL;
    if (cJSON_AddStringToObject(result, "channel", channel) == NULL ||
        (transfers = cJSON_AddArrayToObject(result, "transfers")) == NULL) {
        return false;
    }
    for (size_t i = 0U; i < count; ++i) {
        cJSON *id = cJSON_CreateNumber((double)transfer_ids[i]);
        if (id == NULL || !cJSON_AddItemToArray(transfers, id)) {
            cJSON_Delete(id);
            return false;
        }
    }
    return true;
}

const char *jetkvm_os_services_file_drop(const cJSON *files, cJSON *result,
                                         void *ctx)
{
    (void)ctx;
    static char s_too_many[64];
    if (!started()) {
        return "no OS Services file transfer is connected";
    }
    const int array_size = cJSON_GetArraySize(files);
    const size_t count = array_size > 0 ? (size_t)array_size : 0U;
    if (count > OSVC_FILE_DROP_MAX) {
        /* As the session words it, without holding every entry first. */
        snprintf(s_too_many, sizeof(s_too_many),
                 "too many files for one drop: %zu (at most %u)", count,
                 OSVC_FILE_DROP_MAX);
        return s_too_many;
    }
    const char *error = NULL;
    osvc_file_entry_t *entries = file_entries(files, count, &error);
    uint32_t *transfer_ids =
        heap_caps_calloc(count > 0U ? count : 1U, sizeof(uint32_t),
                         MALLOC_CAP_SPIRAM);
    if (entries == NULL || transfer_ids == NULL) {
        heap_caps_free(entries);
        heap_caps_free(transfer_ids);
        return error != NULL ? error : "out of memory";
    }
    drop_call_t *call = &s_osvc.drop;
    if (xSemaphoreTake(call->serial, pdMS_TO_TICKS(FILE_DROP_WAIT_MS)) !=
        pdTRUE) {
        heap_caps_free(entries);
        heap_caps_free(transfer_ids);
        return "a file transfer is already in progress";
    }
    xSemaphoreTake(call->lock, portMAX_DELAY);
    call->active = true;
    call->finished = false;
    call->files = entries;
    call->count = count;
    call->transfer_ids = transfer_ids;
    call->drop_id = 0U;
    call->error[0] = '\0';
    (void)xSemaphoreTake(call->done, 0);
    xSemaphoreGive(call->lock);

    send_command(COMMAND_FILE_DROP);
    (void)xSemaphoreTake(call->done, pdMS_TO_TICKS(FILE_DROP_WAIT_MS));

    /* After this the task no longer touches the entries or the IDs. */
    xSemaphoreTake(call->lock, portMAX_DELAY);
    call->active = false;
    const bool finished = call->finished;
    const uint32_t drop_id = call->drop_id;
    static char s_error[sizeof(call->error)];
    if (finished && drop_id == 0U) {
        memcpy(s_error, call->error, sizeof(s_error));
    }
    xSemaphoreGive(call->lock);
    xSemaphoreGive(call->serial);

    if (!finished) {
        error = "OS Services did not answer in time";
    } else if (drop_id == 0U) {
        /* Only this caller writes it, and the serial lock is free now; the
         * RPC copies the reason before another drop can start. */
        error = s_error;
    } else if (!add_drop_result(result, drop_id, transfer_ids, count)) {
        error = "out of memory";
    }
    heap_caps_free(entries);
    heap_caps_free(transfer_ids);
    return error;
}

/* --- Channels ------------------------------------------------------------ */

static void channels_lock(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_osvc.channels_lock, portMAX_DELAY);
}

static void channels_unlock(void *ctx)
{
    (void)ctx;
    xSemaphoreGive(s_osvc.channels_lock);
}

/* A channel's buffers (up to 192 KiB) or a final path. */
static void *channels_alloc(void *ctx, size_t size)
{
    (void)ctx;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (size < CHANNEL_ADMISSION_BYTES) {
        return heap_caps_malloc(size, caps);
    }
    jetkvm_memory_policy_reservation_t reservation = {0};
    if (!jetkvm_memory_policy_reserve_psram("OS Services channel", size,
                                            &reservation)) {
        return NULL;
    }
    void *memory = heap_caps_malloc(size, caps);
    jetkvm_memory_policy_release(&reservation);
    return memory;
}

static void channels_free(void *ctx, void *memory)
{
    (void)ctx;
    heap_caps_free(memory);
}

static int64_t channels_now_ms(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time() / 1000;
}

static void channels_service_pending(void *ctx)
{
    (void)ctx;
    xTaskNotifyGive(s_osvc.task);
}

static void channels_output_pending(void *ctx)
{
    (void)ctx;
    const jetkvm_os_services_listener_t listener = current_listener();
    if (listener.channel_output != NULL) {
        listener.channel_output(listener.ctx);
    }
}

bool jetkvm_os_services_channel_claim(uint32_t channel, const char *label)
{
    return started() && osvc_channels_claim(s_osvc.channels, channel, label);
}

void jetkvm_os_services_channel_message(uint32_t channel, bool text,
                                        const uint8_t *data, size_t length)
{
    if (started()) {
        osvc_channels_message(s_osvc.channels, channel, text, data, length);
    }
}

void jetkvm_os_services_channel_closed(uint32_t channel)
{
    if (started()) {
        osvc_channels_closed(s_osvc.channels, channel);
    }
}

bool jetkvm_os_services_channel_next_output(
    jetkvm_os_services_channel_output_t *output)
{
    osvc_channel_output_t next;
    if (!started() || !osvc_channels_next_output(s_osvc.channels, &next)) {
        return false;
    }
    *output = (jetkvm_os_services_channel_output_t) {
        .channel = next.channel,
        .close = next.close,
        .text = next.text,
        .data = next.data,
        .length = next.length,
    };
    return true;
}

void jetkvm_os_services_channel_output_done(uint32_t channel, bool sent)
{
    if (started()) {
        osvc_channels_output_done(s_osvc.channels, channel, sent);
    }
}

/* --- Session platform --------------------------------------------------- */

static int64_t platform_now_ms(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time() / 1000;
}

static void platform_fill_random(void *ctx, uint8_t *out, size_t length)
{
    (void)ctx;
    esp_fill_random(out, length);
}

static bool platform_public_key(void *ctx, uint8_t key[OSVC_PUBLIC_KEY_LEN])
{
    (void)ctx;
    return osvc_identity_public_key(key);
}

static bool platform_sign(void *ctx, const uint8_t *message, size_t length,
                          uint8_t signature[OSVC_SIGNATURE_LEN])
{
    (void)ctx;
    return osvc_identity_sign(message, length, signature);
}

static bool platform_write(void *ctx, const uint8_t envelope[OSVC_ENVELOPE_LEN],
                           const uint8_t *head, size_t head_length,
                           const uint8_t *body, size_t body_length)
{
    (void)ctx;
    const uint8_t *const parts[] = {envelope, head, body};
    const size_t lengths[] = {OSVC_ENVELOPE_LEN, head_length, body_length};
    return osvc_usb_write(parts, lengths, 3U, WRITE_TIMEOUT_MS);
}

static void platform_session_ended(void *ctx, const char *reason)
{
    (void)ctx;
    osvc_usb_stop();
    ESP_LOGI(TAG, "session ended: %s", reason != NULL ? reason : "");
    s_osvc.view_dirty = true;
}

static void platform_reset_transport(void *ctx)
{
    (void)ctx;
    osvc_usb_restart();
}

static void platform_state_changed(void *ctx)
{
    (void)ctx;
    s_osvc.view_dirty = true;
}

static bool platform_terminal_output(void *ctx, uint32_t terminal_id,
                                     const uint8_t *data, size_t length)
{
    (void)ctx;
    return osvc_channels_terminal_output(s_osvc.channels, terminal_id, data,
                                         length);
}

static void platform_terminal_closed(void *ctx, uint32_t terminal_id,
                                     const char *reason, bool exited)
{
    (void)ctx;
    (void)exited;
    osvc_channels_terminal_closed(s_osvc.channels, terminal_id, reason);
}

static void platform_file_credit(void *ctx, uint32_t drop_id,
                                 uint32_t transfer_id, uint64_t bytes)
{
    (void)ctx;
    osvc_channels_file_credit(s_osvc.channels, drop_id, transfer_id, bytes);
}

static void platform_file_done(void *ctx, uint32_t drop_id,
                               uint32_t transfer_id, osvc_file_status_t status,
                               const char *path, size_t path_length, bool last)
{
    (void)ctx;
    osvc_channels_file_done(s_osvc.channels, drop_id, transfer_id, status,
                            path, path_length, last);
}


/* --- Service video ------------------------------------------------------ */

/* The guest display's EDID mode for a size; false for any other size. */
static bool guest_resolution(uint32_t width, uint32_t height,
                             osvc_guest_resolution_t *resolution)
{
    static const struct {
        uint32_t width;
        uint32_t height;
        osvc_guest_resolution_t resolution;
    } modes[] = {
        {1280U, 720U, OSVC_GUEST_RES_720P60},
        {1920U, 1080U, OSVC_GUEST_RES_1080P60},
        {2560U, 1440U, OSVC_GUEST_RES_1440P60},
        {3840U, 2160U, OSVC_GUEST_RES_2160P60},
    };
    for (size_t i = 0U; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        if (modes[i].width == width && modes[i].height == height) {
            *resolution = modes[i].resolution;
            return true;
        }
    }
    return false;
}

static int64_t platform_now_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

static bool platform_display_present(void *ctx, uint32_t width,
                                     uint32_t height, uint32_t serial)
{
    (void)ctx;
    osvc_guest_resolution_t resolution;
    uint8_t edid[OSVC_EDID_SIZE];
    /* As in platform_video_stopped(): the capture task clears the mode
     * once the display is queued, maybe before the next refresh_view(). */
    webrtc_transport_set_video_switching(true);
    return guest_resolution(width, height, &resolution) &&
           osvc_edid_build(resolution, serial, edid) &&
           jetkvm_video_present_guest(edid) == ESP_OK;
}

/* The transport's slots take capture buffers the guest display leaves idle:
 * keyframes of any guest size fit them, and no memory is added for it. */
static bool borrow_capture_buffers(void)
{
    uint8_t *buffers[CONFIG_WEBRTC_TRANSPORT_AU_SLOTS];
    size_t bytes = 0U;
    if (jetkvm_video_lend_guest_buffers(buffers,
                                        CONFIG_WEBRTC_TRANSPORT_AU_SLOTS,
                                        &bytes) != ESP_OK) {
        return false;
    }
    if (webrtc_transport_set_guest_buffers(buffers, bytes,
                                           BUFFER_SWITCH_TIMEOUT_MS) != ESP_OK) {
        jetkvm_video_return_guest_buffers();
        return false;
    }
    s_video.lent_bytes = bytes & ~(size_t)127U;
    return true;
}

/* Hardware capture stays down until the buffers are back. */
static void return_capture_buffers(void)
{
    if (s_video.lent_bytes == 0U) {
        return;
    }
    const esp_err_t error = webrtc_transport_set_guest_buffers(
        NULL, 0U, BUFFER_SWITCH_TIMEOUT_MS);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "the transport still uses the lent capture buffers: %s",
                 esp_err_to_name(error));
        return;
    }
    jetkvm_video_return_guest_buffers();
    s_video.lent_bytes = 0U;
}

static void platform_display_restore(void *ctx)
{
    (void)ctx;
    return_capture_buffers();
    const esp_err_t error = jetkvm_video_restore_physical();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "hardware capture restore refused: %s",
                 esp_err_to_name(error));
    }
}

static size_t platform_video_stream(void *ctx, uint32_t width,
                                    uint32_t height, uint32_t fps)
{
    (void)ctx;
    if (s_video.lent_bytes == 0U && !borrow_capture_buffers()) {
        return 0U;
    }
    return webrtc_transport_set_guest_mode((uint16_t)width, (uint16_t)height,
                                           (uint16_t)fps) == ESP_OK ?
               s_video.lent_bytes : 0U;
}

static void platform_video_stopped(void *ctx)
{
    (void)ctx;
    /* The peer task must see the switch before the cleared mode, which it
     * would otherwise report as no_signal. */
    webrtc_transport_set_video_switching(true);
    webrtc_transport_clear_mode();
    return_capture_buffers();
}

static uint8_t *platform_frame_reserve(void *ctx, size_t length, bool *busy)
{
    (void)ctx;
    const osvc_session_t *session = s_osvc.session;
    const esp_err_t error = webrtc_transport_reserve_guest_access_unit(
        (uint16_t)session->video_width, (uint16_t)session->video_height,
        (uint16_t)session->video_fps, &s_video.lease);
    *busy = error == ESP_ERR_NO_MEM;
    /* The slots went back to their own buffers under a stream sized for
     * the lent ones: skip the frame rather than write past the slot. */
    if (error == ESP_OK && length > s_video.lease.capacity) {
        webrtc_transport_cancel_cpu_access_unit(&s_video.lease, 0U);
        return NULL;
    }
    return error == ESP_OK ? s_video.lease.data : NULL;
}

static bool platform_frame_commit(void *ctx, const osvc_video_frame_t *frame)
{
    (void)ctx;
    /* Stage ages start at the frame's arrival; "encoded" is its last byte. */
    const app_h264_frame_meta_t meta = {
        .sequence = ++s_video.frame_sequence,
        .capture_sequence = s_video.frame_sequence,
        .capture_timestamp_us = frame->arrival_us,
        .capture_eof_timestamp_us = frame->arrival_us,
        .encode_start_timestamp_us = frame->arrival_us,
        .encode_done_timestamp_us = frame->received_us,
        .rtp_timestamp_90khz = frame->pts_90k,
        .sync_idr_generation = frame->keyframe_generation,
        .width = (uint16_t)frame->width,
        .height = (uint16_t)frame->height,
        .fps = (uint16_t)frame->fps,
        .keyframe = frame->keyframe,
    };
    return webrtc_transport_commit_cpu_access_unit(&s_video.lease,
                                                   frame->length, &meta);
}

static void platform_frame_cancel(void *ctx, size_t written)
{
    (void)ctx;
    webrtc_transport_cancel_cpu_access_unit(&s_video.lease, written);
}

static void platform_video_gap(void *ctx)
{
    (void)ctx;
    webrtc_transport_note_video_gap();
}

static uint32_t platform_keyframe_wanted(void *ctx)
{
    (void)ctx;
    uint32_t generation = 0U;
    return webrtc_transport_take_sync_idr_request(&generation) ? generation :
                                                                 0U;
}

static void platform_keyframe_done(void *ctx, uint32_t generation,
                                   bool delivered)
{
    (void)ctx;
    webrtc_transport_complete_sync_idr_production(generation, delivered);
}

/* Service video follows the network as the KVM's own encoder does: only in
 * video quality Auto (jetkvm_video.c). With a manual quality the transport
 * still reports each interval and what it sent, for the session's own
 * limit. */
static void platform_video_network(void *ctx, uint32_t ceiling_kbps,
                                   osvc_video_network_t *network)
{
    (void)ctx;
    uint32_t target_bps = 0U;
    uint32_t sent_bps = 0U;
    network->adaptive = jetkvm_video_quality_get_permille() ==
                        JETKVM_VIDEO_QUALITY_AUTO_PERMILLE;
    network->interval = webrtc_transport_guest_bitrate(
        ceiling_kbps * 1000U, network->adaptive, &target_bps, &sent_bps,
        &network->cut);
    network->estimate_kbps = target_bps / 1000U;
    network->sent_kbps = sent_bps / 1000U;
}

/* For /webrtc/stats, on the HTTP server's task. */
static bool add_guest_video_stats(cJSON *object)
{
    xSemaphoreTake(s_osvc.view_lock, portMAX_DELAY);
    const osvc_video_counters_t c = s_video.stats;
    const osvc_video_source_t source = s_video.stats_source;
    const osvc_video_rate_t rate = s_video.stats_rate;
    const uint32_t sender_kbps = s_video.stats_sender_kbps;
    xSemaphoreGive(s_osvc.view_lock);
    /* The adapted target the KVM asks for and the one the sender reports. */
    static const char *const names[] = {
        "frames", "keyframes", "framesDiscarded", "framesDropped",
        "slotWaits", "slotWaitMaxMs", "slotWaitMs", "keyframeRequests",
        "parameterSetsAdded", "streamResets", "switches", "switchFailures",
        "faults", "largestKeyframe", "targetKbps", "fpsHalved", "rateCuts",
        "rateRaises", "senderKbps", "senderSkippedFrames", "slotWaitTickWakeups",
    };
    const uint32_t values[] = {
        c.frames, c.keyframes, c.frames_discarded, c.frames_dropped,
        c.slot_waits, c.slot_wait_max_ms, c.slot_wait_ms, c.keyframe_requests,
        c.parameter_sets_added, c.stream_resets, c.switches,
        c.switch_failures, c.faults, c.largest_keyframe, rate.kbps,
        rate.low_fps, rate.cuts, rate.raises, sender_kbps,
        c.sender_skipped_frames, c.slot_wait_tick_wakeups,
    };
    bool ok = cJSON_AddStringToObject(object, "source",
                                      osvc_video_source_name(source)) &&
              cJSON_AddNumberToObject(object, "bytes", (double)c.bytes);
    for (size_t i = 0U; ok && i < sizeof(values) / sizeof(values[0]); ++i) {
        ok = cJSON_AddNumberToObject(object, names[i], values[i]) != NULL;
    }
    /* How promptly this task reads the frames off the bus. */
    osvc_usb_counters_t usb;
    osvc_usb_get_counters(&usb);
    return ok &&
           cJSON_AddNumberToObject(object, "usbReadWaitMaxUs",
                                   usb.read_wait_max_us) &&
           cJSON_AddNumberToObject(object, "usbSlowReads", usb.slow_reads);
}

static void publish_stats(void)
{
    const osvc_video_rate_t *rate = &s_osvc.session->video_rate;
    if (rate->kbps != s_video.stats_rate.kbps ||
        rate->low_fps != s_video.stats_rate.low_fps) {
        ESP_LOGI(TAG, "service video target %" PRIu32 " kbps%s (was %" PRIu32
                 " kbps; %" PRIu32 " cuts, %" PRIu32 " raises)",
                 rate->kbps, rate->low_fps ? " at half the frame rate" : "",
                 s_video.stats_rate.kbps, rate->cuts, rate->raises);
    }
    xSemaphoreTake(s_osvc.view_lock, portMAX_DELAY);
    s_video.stats = s_osvc.session->video_counters;
    s_video.stats_source = s_osvc.session->video_source;
    s_video.stats_rate = *rate;
    s_video.stats_sender_kbps = s_osvc.session->video_sender_kbps;
    xSemaphoreGive(s_osvc.view_lock);
}

/* --- The task ----------------------------------------------------------- */

/* On every pass, so no change waits for a command that can be dropped. */
static void apply_settings(void)
{
    xSemaphoreTake(s_osvc.settings_lock, portMAX_DELAY);
    const bool changed = s_osvc.applied_sequence != s_osvc.settings_sequence;
    s_osvc.applied_sequence = s_osvc.settings_sequence;
    const osvc_bridge_settings_t settings = s_osvc.settings;
    const bool video_turned_off = s_osvc.video_turned_off;
    s_osvc.video_turned_off = false;
    xSemaphoreGive(s_osvc.settings_lock);
    if (!changed) {
        return;
    }
    osvc_settings_t session_settings;
    osvc_bridge_settings_session(&settings, &session_settings);
    /* Video turned off and on between two passes still clears the retry
     * budget, as it does when the session sees both changes. */
    if (video_turned_off && session_settings.video_enabled) {
        osvc_settings_t video_off = session_settings;
        video_off.video_enabled = false;
        osvc_session_apply_settings(s_osvc.session, &video_off);
    }
    osvc_session_apply_settings(s_osvc.session, &session_settings);
    s_osvc.view_dirty = true;
}

/* Gives the held OUT read back to the transport. */
static void release_held(void)
{
    if (s_video.held != NULL) {
        s_video.held = NULL;
        s_video.held_length = 0U;
        osvc_usb_receive_done();
    }
}

static void handle_usb_events(void)
{
    osvc_usb_events_t events;
    osvc_usb_get_events(&events);
    if (events.reconfigure_sequence != s_osvc.seen_reconfigure ||
        events.reset_sequence != s_osvc.seen_reset) {
        /* What the read holds belongs to the session that ends now. */
        release_held();
    }
    if (events.reconfigure_sequence != s_osvc.seen_reconfigure) {
        /* By the KVM when its restart was the first reconfiguration since
         * the last one seen. */
        const bool by_kvm = events.restart_reconfigure_sequence ==
                            s_osvc.seen_reconfigure + 1U;
        s_osvc.seen_reconfigure = events.reconfigure_sequence;
        osvc_session_usb_reconfigured(s_osvc.session, by_kvm);
        s_osvc.view_dirty = true;
    }
    if (events.reset_sequence != s_osvc.seen_reset) {
        s_osvc.seen_reset = events.reset_sequence;
        /* A reconfiguration after the RESET ended that session already. */
        if (events.configured &&
            events.reset_reconfigure_sequence == events.reconfigure_sequence) {
            uint8_t status[OSVC_USB_STATUS_LEN];
            osvc_session_usb_reset_with_token(s_osvc.session, events.token,
                                              status);
            /* The transport cannot cancel a transfer already on the bus;
             * the sender discards what it carries while it searches for
             * session.ready, so the session is admitted at once. */
            osvc_session_transport_drained(s_osvc.session);
        }
        s_osvc.view_dirty = true;
    }
}

static void handle_commands(void)
{
    command_t command;
    while (xQueueReceive(s_osvc.commands, &command, 0) == pdTRUE) {
        switch (command) {
        case COMMAND_CLIPBOARD:
            start_clipboard_request();
            break;
        case COMMAND_FILE_DROP:
            start_file_drop();
            break;
        }
    }
}

/* Returns true when reads are still waiting. A frame that waits for a
 * slot, or held shell output, keeps its read until the next call. */
static bool receive(void)
{
    for (unsigned i = 0U; i < RECEIVE_BUDGET; ++i) {
        if (s_video.held == NULL &&
            !osvc_usb_receive(&s_video.held, &s_video.held_length)) {
            return false;
        }
        const size_t used = osvc_session_receive(
            s_osvc.session, s_video.held, s_video.held_length);
        s_video.held += used;
        s_video.held_length -= used;
        /* A frame that waits for a slot, or shell output for a backed-up
         * browser channel: the rest of the read waits, and the bus NAKs. */
        if (s_video.held_length > 0U &&
            (osvc_session_frame_waiting(s_osvc.session) ||
             osvc_session_holding(s_osvc.session))) {
            return false;
        }
        release_held();
    }
    return true;
}

static void service_task(void *arg)
{
    (void)arg;
    while (!started()) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    bool more = false;
    for (;;) {
        /* A waiting frame is woken by the slot's release, or retries each
         * tick. */
        const bool slot_wait = !more && osvc_session_frame_waiting(s_osvc.session);
        const TickType_t wait =
            more ? 0 : slot_wait ? 1 : pdMS_TO_TICKS(TICK_MS);
        if (ulTaskNotifyTake(pdTRUE, wait) == 0U && slot_wait) {
            s_osvc.session->video_counters.slot_wait_tick_wakeups++;
        }
        handle_usb_events();
        apply_settings();
        handle_commands();
        more = receive();
        /* The browser's keystrokes, file bytes, opens and closes. */
        if (osvc_channels_service(s_osvc.channels, s_osvc.session)) {
            more = true;
        }
        if (osvc_session_open(s_osvc.session) &&
            osvc_usb_in_waiting_ms() > IN_UNREAD_LIMIT_MS) {
            osvc_session_end(s_osvc.session,
                             "the sender did not read for 2 seconds");
        }
        osvc_session_display_held(s_osvc.session,
                                  jetkvm_video_capture_suspended());
        osvc_session_display_status(s_osvc.session,
                                    jetkvm_video_guest_display_presented(),
                                    jetkvm_video_guest_owns_display());
        /* A return that timed out at the stop and the restore. */
        if (!jetkvm_video_guest_owns_display()) {
            return_capture_buffers();
        }
        osvc_session_tick(s_osvc.session);
        if (osvc_relay_service(s_osvc.session)) {
            s_osvc.view_dirty = true;
        }
        publish_stats();
        uint8_t status[OSVC_USB_STATUS_LEN];
        osvc_session_usb_status(s_osvc.session, status);
        osvc_usb_publish_status(s_osvc.seen_reset, status);
        if (s_osvc.view_dirty) {
            s_osvc.view_dirty = false;
            refresh_view();
        }
    }
}

esp_err_t jetkvm_os_services_init(void)
{
    if (started() || s_osvc.task != NULL) {
        return ESP_OK;
    }
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_osvc.settings_lock = xSemaphoreCreateMutex();
    s_osvc.view_lock = xSemaphoreCreateMutex();
    s_osvc.call.serial = xSemaphoreCreateMutex();
    s_osvc.call.lock = xSemaphoreCreateRecursiveMutex();
    s_osvc.call.done = xSemaphoreCreateBinary();
    s_osvc.drop.serial = xSemaphoreCreateMutex();
    s_osvc.drop.lock = xSemaphoreCreateMutex();
    s_osvc.drop.done = xSemaphoreCreateBinary();
    s_osvc.channels_lock = xSemaphoreCreateMutex();
    s_osvc.channels = heap_caps_calloc(1U, sizeof(osvc_channels_t), psram);
    s_osvc.commands = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(command_t));
    s_osvc.session = heap_caps_calloc(1U, sizeof(osvc_session_t), psram);
    s_osvc.payload = heap_caps_malloc(OSVC_MAX_CONTROL_PAYLOAD, psram);
    s_osvc.view = heap_caps_calloc(1U, sizeof(view_t), psram);
    s_osvc.candidate = heap_caps_calloc(1U, sizeof(view_t), psram);
    if (s_osvc.settings_lock == NULL || s_osvc.view_lock == NULL ||
        s_osvc.call.serial == NULL || s_osvc.call.lock == NULL ||
        s_osvc.call.done == NULL || s_osvc.drop.serial == NULL ||
        s_osvc.drop.lock == NULL || s_osvc.drop.done == NULL ||
        s_osvc.channels_lock == NULL || s_osvc.channels == NULL ||
        s_osvc.commands == NULL ||
        s_osvc.session == NULL || s_osvc.payload == NULL ||
        s_osvc.view == NULL || s_osvc.candidate == NULL) {
        ESP_LOGE(TAG, "no memory for OS Services");
        /* The interface still enumerates; without the transport's hooks
         * its requests stall and the sender reports the KVM unsupported. */
        return ESP_ERR_NO_MEM;
    }

    load_settings(&s_osvc.settings);
    const esp_err_t identity_error = osvc_identity_load();

    const osvc_channels_platform_t channels = {
        .lock = channels_lock,
        .unlock = channels_unlock,
        .alloc = channels_alloc,
        .free = channels_free,
        .now_ms = channels_now_ms,
        .service_pending = channels_service_pending,
        .output_pending = channels_output_pending,
    };
    osvc_channels_init(s_osvc.channels, &channels);

    const osvc_platform_t platform = {
        .ctx = NULL,
        .now_ms = platform_now_ms,
        .fill_random = platform_fill_random,
        .public_key = platform_public_key,
        .sign = platform_sign,
        .write = platform_write,
        .session_ended = platform_session_ended,
        .reset_transport = platform_reset_transport,
        .state_changed = platform_state_changed,
        .clipboard_done = clipboard_done,
        .terminal_output = platform_terminal_output,
        .terminal_closed = platform_terminal_closed,
        .file_credit = platform_file_credit,
        .file_done = platform_file_done,
        .now_us = platform_now_us,
        .display_present = platform_display_present,
        .display_restore = platform_display_restore,
        .video_stream = platform_video_stream,
        .video_stopped = platform_video_stopped,
        .frame_reserve = platform_frame_reserve,
        .frame_commit = platform_frame_commit,
        .frame_cancel = platform_frame_cancel,
        .video_gap = platform_video_gap,
        .keyframe_wanted = platform_keyframe_wanted,
        .keyframe_done = platform_keyframe_done,
        .video_network = platform_video_network,
    };
    osvc_settings_t session_settings;
    osvc_bridge_settings_session(&s_osvc.settings, &session_settings);
    osvc_session_init(s_osvc.session, &platform, &session_settings,
                      s_osvc.payload, OSVC_MAX_CONTROL_PAYLOAD);
    char fingerprint[OSVC_FINGERPRINT_TEXT_CAPACITY];
    osvc_identity_fingerprint(fingerprint);
    osvc_session_set_fingerprint(s_osvc.session, fingerprint);
    if (identity_error != ESP_OK) {
        osvc_session_set_faulted(s_osvc.session, true);
    }
    fill_view(s_osvc.view);
    webrtc_transport_set_guest_video_source(add_guest_video_stats);

    if (xTaskCreatePinnedToCoreWithCaps(service_task, "os_services",
                                        TASK_STACK_BYTES, NULL, TASK_PRIORITY,
                                        &s_osvc.task, TASK_CORE,
                                        psram) != pdPASS) {
        ESP_LOGE(TAG, "OS Services task could not start");
        return ESP_ERR_NO_MEM;
    }
    /* The task waits for started; until then the session is ours. */
    const esp_err_t relay_error = osvc_relay_init(s_osvc.task);
    if (relay_error != ESP_OK) {
        ESP_LOGW(TAG, "no OS Services update relay");
    }
    const esp_err_t usb_error = osvc_usb_init(s_osvc.task);
    if (usb_error != ESP_OK) {
        ESP_LOGE(TAG, "OS Services USB transport unavailable: %s",
                 esp_err_to_name(usb_error));
        osvc_session_set_faulted(s_osvc.session, true);
    }
    __atomic_store_n(&s_osvc.started, true, __ATOMIC_RELEASE);
    xTaskNotifyGive(s_osvc.task);
    ESP_LOGI(TAG, "OS Services ready (fingerprint %s)", fingerprint);
    return usb_error != ESP_OK ? usb_error : identity_error;
}
