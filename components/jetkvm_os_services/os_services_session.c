/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define HELLO_MESSAGE_LEN (OSVC_ENVELOPE_LEN + OSVC_HELLO_LEN)
#define HELLO_TOKEN_OFFSET (OSVC_ENVELOPE_LEN + 4U)

/* What the browser's terminal shows when its session ends. "The shell
 * exited" is the one success. */
#define TERMINAL_SHELL_EXITED "The shell exited"
#define TERMINAL_SHELL_LOST "The target computer lost its shell"
#define TERMINAL_DISCONNECTED "OS Services disconnected"
#define TERMINAL_DISABLED "Terminal disabled"
#define TERMINAL_UNAVAILABLE "no OS Services terminal is connected"
#define TERMINAL_NOT_OPENED "Terminal unavailable: "
#define TERMINAL_NO_ANSWER TERMINAL_NOT_OPENED "the target computer did not answer"
#define TERMINAL_BUSY \
    TERMINAL_NOT_OPENED "the target computer already runs its maximum number of terminals"
#define TERMINAL_NOT_SENT TERMINAL_NOT_OPENED "the IN endpoint did not drain"
#define TERMINAL_SLOW_BROWSER "Terminal closed: the browser did not keep up"
#define TERMINAL_INPUT_LOST OSVC_TERMINAL_INPUT_LOST
#define TERMINAL_WITHDRAWN "Terminal closed: the target computer withdrew its shell"
#define TERMINAL_FAULTED "Terminal faulted: "

#define FILE_UNAVAILABLE "no OS Services file transfer is connected"
#define FILE_BUSY "a file transfer is already in progress"
#define FILE_NO_FILES "file.drop needs at least one file"

#define IN_ENDPOINT_STUCK "the IN endpoint did not drain"

#define UPDATE_UNAVAILABLE \
    "no OS Services sender that installs updates is connected"
#define UPDATE_BUSY "an OS Services update is already in progress"
#define VIDEO_BLOCKED \
    "OS Services video switched too often; turn video off and on to try again"
#define VIDEO_NOT_PRESENTED "the KVM display could not be presented"
#define VIDEO_NO_STATUS "the sender did not start video within 45 seconds"
#define VIDEO_TOO_LARGE \
    "the KVM display size is above the sender's video.capabilities"

static void end_session(osvc_session_t *s, const char *reason);
static bool send_message(osvc_session_t *s, uint16_t service, uint16_t kind,
                         const uint8_t *payload, size_t length);
static void video_restore(osvc_session_t *s);
static void video_refund_switch(osvc_session_t *s);
static void frame_release(osvc_session_t *s);

static const uint8_t s_hello_envelope[OSVC_ENVELOPE_LEN] = {
    OSVC_SERVICE_SESSION, 0x00, OSVC_SESSION_HELLO, 0x00,
    (uint8_t)OSVC_HELLO_LEN, 0x00, 0x00, 0x00,
};

static int64_t now_ms(const osvc_session_t *s)
{
    return s->platform.now_ms(s->platform.ctx);
}

static void notify_state(osvc_session_t *s)
{
    if (s->platform.state_changed) {
        s->platform.state_changed(s->platform.ctx);
    }
}

static void fail_clipboard_requests(osvc_session_t *s,
                                    osvc_clipboard_status_t status)
{
    for (size_t i = 0U; i < OSVC_CLIPBOARD_PENDING_MAX; ++i) {
        osvc_clipboard_request_t *request = &s->clipboard_requests[i];
        if (!request->in_use) {
            continue;
        }
        request->in_use = false;
        if (s->platform.clipboard_done) {
            s->platform.clipboard_done(s->platform.ctx, request->request_id,
                                       status, false, NULL, 0U, false);
        }
    }
}

static osvc_terminal_t *find_terminal(osvc_session_t *s, uint32_t terminal_id)
{
    if (terminal_id == 0U) {
        return NULL;
    }
    for (size_t i = 0U; i < OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        if (s->terminals[i].state != OSVC_TERMINAL_FREE &&
            s->terminals[i].terminal_id == terminal_id) {
            return &s->terminals[i];
        }
    }
    return NULL;
}

static bool send_terminal_close(osvc_session_t *s, uint32_t terminal_id)
{
    uint8_t payload[4];
    osvc_put_u32(payload, terminal_id);
    return send_message(s, OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_CLOSE, payload,
                        sizeof(payload));
}

/*
 * Ends one terminal session on the KVM's side and reports it. The slot is
 * freed first, so neither the report nor a close whose write fails (which
 * ends the session) can end it twice. close_sender sends terminal.close.
 */
static void end_terminal(osvc_session_t *s, osvc_terminal_t *terminal,
                         const char *reason, bool exited, bool close_sender)
{
    const uint32_t terminal_id = terminal->terminal_id;
    memset(terminal, 0, sizeof(*terminal));
    if (s->platform.terminal_closed) {
        s->platform.terminal_closed(s->platform.ctx, terminal_id, reason,
                                    exited);
    }
    if (close_sender && s->link == OSVC_LINK_OPEN) {
        (void)send_terminal_close(s, terminal_id);
    }
}

static void end_terminals(osvc_session_t *s, const char *reason,
                          bool close_sender)
{
    for (size_t i = 0U; i < OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        if (s->terminals[i].state != OSVC_TERMINAL_FREE) {
            end_terminal(s, &s->terminals[i], reason, false, close_sender);
        }
    }
}

static uint32_t transfer_id_at(const osvc_file_drop_t *drop, size_t index)
{
    return drop->drop_id + 1U + (uint32_t)index;
}

/* The index of a transfer of the drop that has not ended. drop_id 0 accepts
 * the current drop, as the sender's messages name no drop. */
static bool find_transfer(const osvc_session_t *s, uint32_t drop_id,
                          uint32_t transfer_id, size_t *index)
{
    const osvc_file_drop_t *drop = &s->file_drop;
    if (drop->drop_id == 0U || (drop_id != 0U && drop_id != drop->drop_id) ||
        transfer_id <= drop->drop_id ||
        transfer_id - drop->drop_id > drop->count) {
        return false;
    }
    const size_t i = transfer_id - drop->drop_id - 1U;
    if (drop->transfers[i].ended) {
        return false;
    }
    *index = i;
    return true;
}

static void forget_drop(osvc_session_t *s)
{
    memset(&s->file_drop, 0, sizeof(s->file_drop));
}

/* Tells the browser the credit it has not heard of yet. */
static void report_credit(osvc_session_t *s, size_t index)
{
    osvc_file_drop_t *drop = &s->file_drop;
    osvc_file_transfer_t *transfer = &drop->transfers[index];
    if (!drop->attached || transfer->ended ||
        transfer->credit == transfer->credit_reported) {
        return;
    }
    const uint64_t bytes = transfer->credit - transfer->credit_reported;
    transfer->credit_reported = transfer->credit;
    if (s->platform.file_credit) {
        s->platform.file_credit(s->platform.ctx, drop->drop_id,
                                transfer_id_at(drop, index), bytes);
    }
}

/* Tells the browser how a transfer ended; after the last one the drop is
 * over. */
static void report_done(osvc_session_t *s, size_t index, const char *path,
                        size_t path_length)
{
    osvc_file_drop_t *drop = &s->file_drop;
    osvc_file_transfer_t *transfer = &drop->transfers[index];
    transfer->reported = true;
    drop->reported++;
    const uint32_t drop_id = drop->drop_id;
    const uint32_t transfer_id = transfer_id_at(drop, index);
    const osvc_file_status_t status = transfer->status;
    const bool last = drop->reported == drop->count;
    if (last) {
        forget_drop(s);
    }
    if (s->platform.file_done) {
        s->platform.file_done(s->platform.ctx, drop_id, transfer_id, status,
                              path != NULL ? path : "",
                              path != NULL ? path_length : 0U, last);
    }
}

/*
 * Ends one transfer on the KVM's side, reports it when the drop's channel is
 * attached, and with cancel_sender sends file.cancel. The transfer is marked
 * ended first, so a cancel whose write fails (which ends the session) cannot
 * end it twice.
 */
static void end_transfer(osvc_session_t *s, size_t index,
                         osvc_file_status_t status, const char *path,
                         size_t path_length, bool cancel_sender)
{
    osvc_file_drop_t *drop = &s->file_drop;
    osvc_file_transfer_t *transfer = &drop->transfers[index];
    const uint32_t transfer_id = transfer_id_at(drop, index);
    transfer->ended = true;
    transfer->status = status;
    drop->ended++;
    if (drop->ended == drop->count) {
        /* An unattached drop keeps its outcomes for the channel this long. */
        drop->progress_ms = now_ms(s);
    }
    if (drop->attached) {
        report_done(s, index, path, path_length);
    }
    if (cancel_sender && s->link == OSVC_LINK_OPEN) {
        uint8_t payload[4];
        osvc_put_u32(payload, transfer_id);
        (void)send_message(s, OSVC_SERVICE_FILE, OSVC_FILE_CANCEL, payload,
                           sizeof(payload));
    }
}

/* Ends every transfer of the drop that is still open. */
static void end_transfers(osvc_session_t *s, osvc_file_status_t status,
                          bool cancel_sender)
{
    const uint32_t drop_id = s->file_drop.drop_id;
    for (size_t i = 0U; drop_id != 0U && s->file_drop.drop_id == drop_id &&
                        i < s->file_drop.count;
         ++i) {
        if (!s->file_drop.transfers[i].ended) {
            end_transfer(s, i, status, NULL, 0U, cancel_sender);
        }
    }
}

/* Forgets the video registration and what the session knew of the stream.
 * The source, the retry budget and the stream clock stay. */
static void clear_video(osvc_session_t *s)
{
    s->video_registered = false;
    s->video_codec_flags = 0U;
    s->video_max_width = 0U;
    s->video_max_height = 0U;
    s->video_max_fps = 0U;
    s->video_switch_wanted = false;
    s->video_answer_pending = false;
    s->video_config_enabled = false;
    s->video_sender_enabled = false;
    s->video_disable_pending = false;
    s->video_stream_started = false;
    s->video_reset_pending = false;
    s->video_error[0] = '\0';
    s->parameter_sets_length = 0U;
}

static void clear_registrations(osvc_session_t *s)
{
    s->clipboard_registered = false;
    memset(&s->clipboard, 0, sizeof(s->clipboard));
    s->terminal_shell = false;
    s->file_receive = false;
    s->update_install = false;
    s->update_package[0] = '\0';
    clear_video(s);
    memset(s->service_faulted, 0, sizeof(s->service_faulted));
}

static bool send_message(osvc_session_t *s, uint16_t service, uint16_t kind,
                         const uint8_t *payload, size_t length);

static bool update_open(const osvc_session_t *s, uint32_t update_id)
{
    return update_id != 0U && s->update.update_id == update_id &&
           !s->update.ended;
}

/* Ends the open update on the KVM's side; cancel_sender also sends
 * update.cancel. The update is marked ended first, so a cancel whose write
 * fails (which ends the session) cannot end it twice. */
static void end_update(osvc_session_t *s, uint8_t status, const char *message,
                       bool cancel_sender)
{
    osvc_update_t *update = &s->update;
    if (!update_open(s, update->update_id)) {
        return;
    }
    update->ended = true;
    update->status = status;
    snprintf(update->message, sizeof(update->message), "%s", message);
    if (cancel_sender && s->link == OSVC_LINK_OPEN) {
        uint8_t payload[4];
        osvc_put_u32(payload, update->update_id);
        (void)send_message(s, OSVC_SERVICE_UPDATE, OSVC_UPDATE_CANCEL, payload,
                           sizeof(payload));
    }
}

/*
 * Ends the current USB session, if any: fails what every service owes,
 * forgets the registrations and tells the transport to stop reading. The
 * link goes to Idle; only a new RESET admits the next session. A KVM that
 * shows service video returns to hardware capture.
 */
static void end_session(osvc_session_t *s, const char *reason)
{
    const bool was_open = s->link == OSVC_LINK_OPEN;
    const bool had_session = s->link != OSVC_LINK_IDLE;
    s->link = OSVC_LINK_IDLE;
    frame_release(s);
    s->parse = OSVC_PARSE_STOPPED;
    if (s->video_switch_back &&
        (s->video_source == OSVC_SOURCE_SWITCHING_TO_GUEST ||
         s->video_source == OSVC_SOURCE_GUEST)) {
        video_refund_switch(s);
    }
    s->video_switch_back = false;
    video_restore(s);
    fail_clipboard_requests(s, OSVC_CLIPBOARD_FAILED);
    end_terminals(s, TERMINAL_DISCONNECTED, false);
    end_transfers(s, OSVC_FILE_DONE_FAILED, false);
    end_update(s, OSVC_UPDATE_STATUS_FAILED, "the OS Services session ended",
               false);
    clear_registrations(s);
    s->sender_info_valid = false;
    s->sender_version[0] = '\0';
    s->display_count = 0U;
    if (had_session) {
        snprintf(s->last_end_reason, sizeof(s->last_end_reason), "%s",
                 reason ? reason : "");
        if (was_open) {
            s->counters.sessions_ended++;
        }
        if (s->platform.session_ended) {
            s->platform.session_ended(s->platform.ctx, reason);
        }
    }
    if (was_open) {
        notify_state(s);
    }
}

/* Queues one message of a head and a body; false when it did not fit. The
 * caller ends the session then. */
static bool write_message(osvc_session_t *s, uint16_t service, uint16_t kind,
                          const uint8_t *head, size_t head_length,
                          const uint8_t *body, size_t body_length)
{
    uint8_t envelope[OSVC_ENVELOPE_LEN];
    osvc_put_envelope(envelope, service, kind,
                      (uint32_t)(head_length + body_length));
    if (!s->platform.write(s->platform.ctx, envelope, head, head_length, body,
                           body_length)) {
        return false;
    }
    s->counters.bytes_out += OSVC_ENVELOPE_LEN + head_length + body_length;
    return true;
}

static bool send_message(osvc_session_t *s, uint16_t service, uint16_t kind,
                         const uint8_t *payload, size_t length)
{
    if (!write_message(s, service, kind, payload, length, NULL, 0U)) {
        end_session(s, IN_ENDPOINT_STUCK);
        return false;
    }
    return true;
}

static bool send_session_config(osvc_session_t *s)
{
    cJSON *config = cJSON_CreateObject();
    char *text = NULL;
    bool ok = config != NULL &&
              cJSON_AddBoolToObject(config, "terminal_enabled",
                                    s->settings.terminal_enabled) != NULL &&
              cJSON_AddBoolToObject(config, "files_enabled",
                                    s->settings.files_enabled) != NULL &&
              cJSON_AddStringToObject(config, "capture_display",
                                      s->settings.capture_display) != NULL &&
              /* This KVM presents the same monitor each time to a sender
               * that sets the display's mode (video_display_serial). */
              cJSON_AddBoolToObject(config, "display_mode_by_sender", true) !=
                  NULL;
    if (ok) {
        text = cJSON_PrintUnformatted(config);
        ok = text != NULL;
    }
    cJSON_Delete(config);
    if (!ok) {
        cJSON_free(text);
        end_session(s, "out of memory for session.config");
        return false;
    }
    ok = send_message(s, OSVC_SERVICE_SESSION, OSVC_SESSION_CONFIG,
                      (const uint8_t *)text, strlen(text));
    cJSON_free(text);
    return ok;
}

static bool send_clipboard_config(osvc_session_t *s)
{
    const uint8_t enabled = s->settings.clipboard_enabled ? 1U : 0U;
    return send_message(s, OSVC_SERVICE_CLIPBOARD, OSVC_CLIPBOARD_CONFIG,
                        &enabled, 1U);
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

/* The static targets: the settings' own, else a bitrate by display height,
 * within the hardware path's ceilings. */
static void video_targets(const osvc_settings_t *settings, uint32_t *kbps,
                          uint32_t *fps)
{
    const uint32_t height = settings->height;
    const uint32_t default_kbps = height <= 720U  ? OSVC_VIDEO_KBPS_720P :
                                  height <= 1080U ? OSVC_VIDEO_KBPS_1080P :
                                                    OSVC_VIDEO_KBPS_1440P;
    *kbps = settings->target_kbps != 0U ?
                min_u32(settings->target_kbps, OSVC_VIDEO_KBPS_MAX) :
                default_kbps;
    *fps = settings->maximum_fps != 0U ?
               min_u32(settings->maximum_fps, OSVC_VIDEO_FPS_MAX) :
               OSVC_VIDEO_FPS_MAX;
}

/* Slot waits count from now. */
static void video_interval_restart(osvc_session_t *s, int64_t now)
{
    s->video_interval_ms = now;
    s->video_interval_wait_ms = 0U;
    s->video_interval_frames = 0U;
    s->video_free_mfps = 0U;
    s->video_pressure_intervals = 0U;
}

static void video_rate_reset(osvc_session_t *s, uint32_t ceiling_kbps,
                             int64_t now)
{
    osvc_video_rate_reset(&s->video_rate, ceiling_kbps, now);
    video_interval_restart(s, now);
}

/* The video settings the sender gets: the KVM display size, layout and
 * codec, and the static targets with the frame-rate cap clamped to what the
 * sender advertised, or the targets adapted below them for these
 * settings. */
static bool send_video_config(osvc_session_t *s, bool enabled)
{
    uint8_t payload[OSVC_VIDEO_CONFIG_LEN] = {0};
    uint32_t target_kbps;
    uint32_t maximum_fps;
    video_targets(&s->settings, &target_kbps, &maximum_fps);
    if (s->video_max_fps != 0U) {
        maximum_fps = min_u32(maximum_fps, s->video_max_fps);
    }
    if (s->settings.target_kbps == 0U &&
        s->video_rate.ceiling_kbps == target_kbps) {
        target_kbps = s->video_rate.kbps;
        maximum_fps = osvc_video_rate_fps(&s->video_rate, maximum_fps);
    }
    payload[0] = enabled ? 1U : 0U;
    payload[1] = (uint8_t)s->settings.layout;
    payload[2] = (uint8_t)OSVC_CODEC_H264;
    osvc_put_u32(payload + 4, s->settings.width);
    osvc_put_u32(payload + 8, s->settings.height);
    osvc_put_u32(payload + 12, target_kbps);
    osvc_put_u32(payload + 16, maximum_fps);
    s->video_answer_pending = false;
    s->video_config_enabled = enabled;
    s->video_sender_enabled |= enabled;
    return send_message(s, OSVC_SERVICE_VIDEO, OSVC_VIDEO_CONFIG, payload,
                        sizeof(payload));
}

/* Detaches a service after an invalid message (Errors): it fails what the
 * service owes and ignores its messages until it registers again. */
static void fault_service(osvc_session_t *s, uint16_t service,
                          const char *reason)
{
    if (service >= OSVC_SERVICE_COUNT || service == OSVC_SERVICE_SESSION) {
        end_session(s, reason);
        return;
    }
    s->counters.service_faults++;
    s->service_faulted[service] = true;
    snprintf(s->last_fault_reason, sizeof(s->last_fault_reason), "%s",
             reason);
    switch (service) {
    case OSVC_SERVICE_CLIPBOARD:
        s->clipboard_registered = false;
        memset(&s->clipboard, 0, sizeof(s->clipboard));
        fail_clipboard_requests(s, OSVC_CLIPBOARD_FAILED);
        break;
    case OSVC_SERVICE_TERMINAL: {
        /* The sender does not know of the fault; terminal.close ends the
         * shells that the KVM no longer serves. */
        s->terminal_shell = false;
        char line[sizeof(TERMINAL_FAULTED) + sizeof(s->last_fault_reason)];
        snprintf(line, sizeof(line), "%s%s", TERMINAL_FAULTED, reason);
        end_terminals(s, line, true);
        break;
    }
    case OSVC_SERVICE_FILE:
        s->file_receive = false;
        end_transfers(s, OSVC_FILE_DONE_FAILED, true);
        break;
    case OSVC_SERVICE_UPDATE:
        s->update_install = false;
        s->update_package[0] = '\0';
        end_update(s, OSVC_UPDATE_STATUS_FAILED, reason, true);
        break;
    case OSVC_SERVICE_VIDEO: {
        /* Back to hardware capture first: a failed write ends the session. */
        const bool registered = s->video_registered;
        s->video_registered = false;
        s->video_switch_wanted = false;
        /* No status is expected: they go unread until video registers
         * again. */
        s->video_sender_enabled = false;
        s->video_counters.faults++;
        snprintf(s->video_error, sizeof(s->video_error), "%s", reason);
        video_restore(s);
        if (registered) {
            (void)send_video_config(s, false);
        }
        break;
    }
    default:
        break;
    }
    notify_state(s);
}

static void sign_and_send_ready(osvc_session_t *s, const uint8_t *hello)
{
    uint8_t ready[OSVC_READY_LEN];
    /* The lower of the two minor versions (Version). */
    const uint32_t minor = osvc_get_u16(hello + 2);
    const uint32_t ours = OSVC_PROTOCOL_MINOR;
    s->protocol_minor = (uint16_t)(minor < ours ? minor : ours);
    osvc_put_u16(ready, (uint16_t)OSVC_PROTOCOL_MAJOR);
    osvc_put_u16(ready + 2, s->protocol_minor);
    memcpy(ready + 4, s->token, OSVC_TOKEN_LEN);
    if (!s->platform.public_key(s->platform.ctx, ready + 20)) {
        end_session(s, "the KVM key is unavailable");
        return;
    }
    /* The signed bytes: the context text, the hello payload and the first
     * 52 bytes of this payload (KVM approval). */
    uint8_t signed_bytes[sizeof(OSVC_SIGNATURE_CONTEXT) - 1U + OSVC_HELLO_LEN +
                         OSVC_READY_SIGNED_LEN];
    size_t offset = 0U;
    memcpy(signed_bytes, OSVC_SIGNATURE_CONTEXT,
           sizeof(OSVC_SIGNATURE_CONTEXT) - 1U);
    offset += sizeof(OSVC_SIGNATURE_CONTEXT) - 1U;
    memcpy(signed_bytes + offset, hello, OSVC_HELLO_LEN);
    offset += OSVC_HELLO_LEN;
    memcpy(signed_bytes + offset, ready, OSVC_READY_SIGNED_LEN);
    offset += OSVC_READY_SIGNED_LEN;
    if (!s->platform.sign(s->platform.ctx, signed_bytes, offset,
                          ready + OSVC_READY_SIGNED_LEN)) {
        end_session(s, "the KVM could not sign session.ready");
        return;
    }
    s->link = OSVC_LINK_OPEN;
    s->last_message_ms = now_ms(s);
    s->counters.last_handshake_ms = s->last_message_ms - s->admitted_ms;
    if (!send_message(s, OSVC_SERVICE_SESSION, OSVC_SESSION_READY, ready,
                      sizeof(ready))) {
        return;
    }
    if (!send_session_config(s)) {
        return;
    }
    s->counters.sessions_opened++;
    s->session_messages = 0U;
    notify_state(s);
}

/* The hello was found with the RESET token; its version decides the rest. */
static void handle_hello(osvc_session_t *s, const uint8_t *hello)
{
    if (osvc_get_u16(hello) != OSVC_PROTOCOL_MAJOR) {
        end_session(s, "session.hello has another protocol major version");
        return;
    }
    sign_and_send_ready(s, hello);
}

static bool json_string_within(const cJSON *item, size_t limit)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           strlen(item->valuestring) <= limit;
}

static void handle_session_info(osvc_session_t *s, const uint8_t *payload,
                                size_t length)
{
    cJSON *info = cJSON_ParseWithLength((const char *)payload, length);
    if (info == NULL || !cJSON_IsObject(info)) {
        cJSON_Delete(info);
        end_session(s, "session.info is not a JSON object");
        return;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(info, "version");
    const cJSON *displays = cJSON_GetObjectItemCaseSensitive(info, "displays");
    bool valid = (version == NULL || cJSON_IsNull(version) ||
                  json_string_within(version, OSVC_INFO_VERSION_MAX)) &&
                 (displays == NULL || cJSON_IsNull(displays) ||
                  (cJSON_IsArray(displays) &&
                   (size_t)cJSON_GetArraySize(displays) <=
                       OSVC_INFO_DISPLAYS_MAX));
    if (valid && cJSON_IsArray(displays)) {
        const cJSON *name;
        cJSON_ArrayForEach(name, displays) {
            if (!json_string_within(name, OSVC_INFO_DISPLAY_MAX)) {
                valid = false;
                break;
            }
        }
    }
    if (!valid) {
        cJSON_Delete(info);
        end_session(s, "session.info is over a limit");
        return;
    }
    /* Compared and copied in place: a copy of the display list would not
     * fit the service task's stack. */
    const char *new_version =
        cJSON_IsString(version) ? version->valuestring : "";
    bool changed = !s->sender_info_valid ||
                   strcmp(new_version, s->sender_version) != 0;
    size_t new_count = 0U;
    size_t used = 0U;
    if (cJSON_IsArray(displays)) {
        const cJSON *name;
        cJSON_ArrayForEach(name, displays) {
            const size_t n = strlen(name->valuestring) + 1U;
            if (!changed &&
                memcmp(s->displays + used, name->valuestring, n) != 0) {
                changed = true;
            }
            used += n;
            new_count++;
        }
    }
    changed = changed || new_count != s->display_count;
    snprintf(s->sender_version, sizeof(s->sender_version), "%s", new_version);
    used = 0U;
    if (cJSON_IsArray(displays)) {
        const cJSON *name;
        cJSON_ArrayForEach(name, displays) {
            const size_t n = strlen(name->valuestring) + 1U;
            memcpy(s->displays + used, name->valuestring, n);
            used += n;
        }
    }
    cJSON_Delete(info);
    s->display_count = new_count;
    s->sender_info_valid = true;
    if (changed) {
        notify_state(s);
    }
}

static void handle_session(osvc_session_t *s, const uint8_t *payload,
                           size_t length)
{
    switch (s->kind) {
    case OSVC_SESSION_HELLO:
        end_session(s, "a second session.hello");
        return;
    case OSVC_SESSION_PING:
        if (length != 0U) {
            end_session(s, "session.ping with a payload");
        }
        return;
    case OSVC_SESSION_CLOSE:
        if (length != 1U || (payload[0] != OSVC_CLOSE_SHUTDOWN &&
                             payload[0] != OSVC_CLOSE_PROTOCOL_ERROR &&
                             payload[0] != OSVC_CLOSE_NOT_APPROVED)) {
            end_session(s, "invalid session.close");
            return;
        }
        if (payload[0] == OSVC_CLOSE_NOT_APPROVED && !s->approval_pending) {
            s->approval_pending = true;
            notify_state(s);
        }
        end_session(s, payload[0] == OSVC_CLOSE_NOT_APPROVED ?
                           "the sender's user has not approved this KVM" :
                       payload[0] == OSVC_CLOSE_PROTOCOL_ERROR ?
                           "the sender closed the session: protocol error" :
                           "the sender closed the session");
        return;
    case OSVC_SESSION_INFO:
        handle_session_info(s, payload, length);
        return;
    case OSVC_SESSION_READY:
    case OSVC_SESSION_CONFIG:
        end_session(s, "a KVM-to-sender session message from the sender");
        return;
    default:
        s->counters.messages_skipped++;
        return;
    }
}

static osvc_clipboard_request_t *find_clipboard_request(osvc_session_t *s,
                                                        uint32_t request_id)
{
    for (size_t i = 0U; i < OSVC_CLIPBOARD_PENDING_MAX; ++i) {
        if (s->clipboard_requests[i].in_use &&
            s->clipboard_requests[i].request_id == request_id) {
            return &s->clipboard_requests[i];
        }
    }
    return NULL;
}

static void finish_clipboard_request(osvc_session_t *s,
                                     osvc_clipboard_request_t *request,
                                     osvc_clipboard_status_t status,
                                     const uint8_t *text, size_t length,
                                     bool truncated)
{
    const uint32_t id = request->request_id;
    request->in_use = false;
    if (s->platform.clipboard_done) {
        s->platform.clipboard_done(s->platform.ctx, id, status, true, text,
                                   length, truncated);
    }
}

static void handle_clipboard(osvc_session_t *s, const uint8_t *payload,
                             size_t length)
{
    switch (s->kind) {
    case OSVC_CLIPBOARD_CAPABILITIES: {
        if (length != 8U) {
            fault_service(s, OSVC_SERVICE_CLIPBOARD,
                          "invalid clipboard.capabilities length");
            return;
        }
        const uint32_t flags = osvc_get_u32(payload);
        if ((flags & ~(OSVC_CLIPBOARD_FLAG_READ | OSVC_CLIPBOARD_FLAG_WRITE)) !=
                0U ||
            payload[4] > OSVC_OS_LINUX || payload[5] != 0U ||
            payload[6] != 0U || payload[7] != 0U) {
            fault_service(s, OSVC_SERVICE_CLIPBOARD,
                          "invalid clipboard.capabilities");
            return;
        }
        s->service_faulted[OSVC_SERVICE_CLIPBOARD] = false;
        s->clipboard.flags = flags;
        s->clipboard.os = (osvc_target_os_t)payload[4];
        s->clipboard_registered = flags != 0U;
        if (flags == 0U) {
            fail_clipboard_requests(s, OSVC_CLIPBOARD_UNSUPPORTED);
        }
        if (send_clipboard_config(s)) {
            notify_state(s);
        }
        return;
    }
    case OSVC_CLIPBOARD_CONTENT: {
        if (length < 8U || payload[4] > OSVC_CLIPBOARD_FAILED ||
            payload[5] > 1U || payload[6] != 0U || payload[7] != 0U ||
            length - 8U > OSVC_CLIPBOARD_MAX_TEXT ||
            (payload[4] != OSVC_CLIPBOARD_OK && length != 8U) ||
            !osvc_utf8_valid(payload + 8, length - 8U)) {
            fault_service(s, OSVC_SERVICE_CLIPBOARD,
                          "invalid clipboard.content");
            return;
        }
        osvc_clipboard_request_t *request =
            find_clipboard_request(s, osvc_get_u32(payload));
        if (request == NULL) {
            return; /* abandoned after 2 seconds; a late answer is ignored */
        }
        if (request->kind != OSVC_CLIPBOARD_GET) {
            fault_service(s, OSVC_SERVICE_CLIPBOARD,
                          "clipboard.content answers a clipboard.set");
            return;
        }
        finish_clipboard_request(s, request,
                                 (osvc_clipboard_status_t)payload[4],
                                 payload + 8, length - 8U, payload[5] == 1U);
        return;
    }
    case OSVC_CLIPBOARD_STATUS: {
        if (length != 5U || payload[4] > OSVC_CLIPBOARD_FAILED) {
            fault_service(s, OSVC_SERVICE_CLIPBOARD,
                          "invalid clipboard.status");
            return;
        }
        osvc_clipboard_request_t *request =
            find_clipboard_request(s, osvc_get_u32(payload));
        if (request == NULL) {
            return;
        }
        if (request->kind != OSVC_CLIPBOARD_SET) {
            fault_service(s, OSVC_SERVICE_CLIPBOARD,
                          "clipboard.status answers a clipboard.get");
            return;
        }
        finish_clipboard_request(s, request,
                                 (osvc_clipboard_status_t)payload[4], NULL, 0U,
                                 false);
        return;
    }
    case OSVC_CLIPBOARD_GET:
    case OSVC_CLIPBOARD_SET:
    case OSVC_CLIPBOARD_CONFIG:
        fault_service(s, OSVC_SERVICE_CLIPBOARD,
                      "a KVM-to-sender clipboard message from the sender");
        return;
    default:
        s->counters.messages_skipped++;
        return;
    }
}

/* terminal.capabilities and file.capabilities: a u32 of flags with one
 * defined bit; 0 withdraws. */
static bool parse_flag_capabilities(osvc_session_t *s, uint16_t service,
                                    const uint8_t *payload, size_t length,
                                    uint32_t defined, bool *registered)
{
    if (length != 4U || (osvc_get_u32(payload) & ~defined) != 0U) {
        fault_service(s, service, "invalid capabilities");
        return false;
    }
    s->service_faulted[service] = false;
    *registered = osvc_get_u32(payload) != 0U;
    notify_state(s);
    return true;
}

static const char *terminal_refusal(uint8_t status)
{
    switch (status) {
    case OSVC_TERMINAL_OPENED_UNSUPPORTED:
        return TERMINAL_NOT_OPENED "the target computer allows no remote terminal";
    case OSVC_TERMINAL_OPENED_BUSY:
        return TERMINAL_BUSY;
    default:
        return TERMINAL_NOT_OPENED "the target computer could not start a shell";
    }
}

/* terminal.opened, terminal.output and terminal.exited: a non-zero
 * session_id, then a status byte or the data. */
static bool terminal_message_valid(uint16_t kind, const uint8_t *payload,
                                   size_t length)
{
    if (length < 4U || osvc_get_u32(payload) == 0U) {
        return false;
    }
    switch (kind) {
    case OSVC_TERMINAL_OPENED:
        return length == OSVC_TERMINAL_STATUS_LEN &&
               payload[4] <= OSVC_TERMINAL_OPENED_FAILED;
    case OSVC_TERMINAL_OUTPUT:
        return length > 4U;
    default:
        return length == OSVC_TERMINAL_STATUS_LEN &&
               payload[4] <= OSVC_TERMINAL_EXIT_FAILED;
    }
}

/* Offers the held terminal.output to the browser again. Returns false while
 * its channel stays backed up; past OSVC_TERMINAL_OUTPUT_HOLD_MS the
 * terminal ends instead, since shell output cannot be dropped without
 * corrupting the screen. */
static bool deliver_held(osvc_session_t *s)
{
    const uint32_t terminal_id = osvc_get_u32(s->payload);
    osvc_terminal_t *terminal = find_terminal(s, terminal_id);
    const bool taken =
        terminal == NULL ||
        s->platform.terminal_output(s->platform.ctx, terminal_id,
                                    s->payload + 4, s->payload_size - 4U);
    if (!taken && now_ms(s) - s->held_ms < OSVC_TERMINAL_OUTPUT_HOLD_MS) {
        return false;
    }
    s->parse = OSVC_PARSE_HEADER;
    if (!taken) {
        end_terminal(s, terminal, TERMINAL_SLOW_BROWSER, false, true);
    }
    return true;
}

static void handle_terminal(osvc_session_t *s, const uint8_t *payload,
                            size_t length)
{
    switch (s->kind) {
    case OSVC_TERMINAL_CAPABILITIES:
        if (parse_flag_capabilities(s, OSVC_SERVICE_TERMINAL, payload, length,
                                    OSVC_TERMINAL_FLAG_SHELL,
                                    &s->terminal_shell) &&
            !s->terminal_shell) {
            /* The sender ends its shells before it withdraws; what is left
             * here never opened there. */
            end_terminals(s, TERMINAL_WITHDRAWN, false);
        }
        return;
    case OSVC_TERMINAL_OPENED:
    case OSVC_TERMINAL_OUTPUT:
    case OSVC_TERMINAL_EXITED:
        break;
    case OSVC_TERMINAL_OPEN:
    case OSVC_TERMINAL_INPUT:
    case OSVC_TERMINAL_RESIZE:
    case OSVC_TERMINAL_CLOSE:
    case OSVC_TERMINAL_CREDIT:
        fault_service(s, OSVC_SERVICE_TERMINAL,
                      "a KVM-to-sender terminal message from the sender");
        return;
    default:
        s->counters.messages_skipped++;
        return;
    }
    if (!terminal_message_valid(s->kind, payload, length)) {
        fault_service(s, OSVC_SERVICE_TERMINAL,
                      s->kind == OSVC_TERMINAL_OPENED ? "invalid terminal.opened" :
                      s->kind == OSVC_TERMINAL_OUTPUT ? "invalid terminal.output" :
                                                        "invalid terminal.exited");
        return;
    }
    const uint32_t terminal_id = osvc_get_u32(payload);
    osvc_terminal_t *terminal = find_terminal(s, terminal_id);
    if (terminal == NULL) {
        /* It ended while the message was in transit (Terminal service). */
        return;
    }
    switch (s->kind) {
    case OSVC_TERMINAL_OPENED:
        if (payload[4] == OSVC_TERMINAL_OPENED_OK) {
            terminal->state = OSVC_TERMINAL_RUNNING;
        } else {
            /* Only OK creates a terminal session: nothing to close there. */
            end_terminal(s, terminal, terminal_refusal(payload[4]), false,
                         false);
        }
        return;
    case OSVC_TERMINAL_OUTPUT:
        /* A terminal opens only on a platform that takes output. The
         * payload stays in s->payload while it is held. */
        if (s->platform.terminal_output != NULL) {
            s->parse = OSVC_PARSE_HELD;
            s->held_ms = now_ms(s);
            (void)deliver_held(s);
        }
        return;
    default:
        end_terminal(s, terminal,
                     payload[4] == OSVC_TERMINAL_EXIT_EXITED ? TERMINAL_SHELL_EXITED :
                     payload[4] == OSVC_TERMINAL_EXIT_FAILED ? TERMINAL_SHELL_LOST :
                                                               "",
                     payload[4] == OSVC_TERMINAL_EXIT_EXITED, false);
        return;
    }
}

static void handle_file_credit(osvc_session_t *s, const uint8_t *payload,
                               size_t length)
{
    const bool ready = s->kind == OSVC_FILE_READY;
    if (length != OSVC_FILE_CREDIT_LEN) {
        fault_service(s, OSVC_SERVICE_FILE,
                      ready ? "invalid file.ready" : "invalid file.credit");
        return;
    }
    size_t index;
    if (!find_transfer(s, 0U, osvc_get_u32(payload), &index)) {
        return; /* ended here while the message was in transit */
    }
    osvc_file_transfer_t *transfer = &s->file_drop.transfers[index];
    if (ready && transfer->ready) {
        fault_service(s, OSVC_SERVICE_FILE, "a second file.ready");
        return;
    }
    if (!ready && !transfer->ready) {
        fault_service(s, OSVC_SERVICE_FILE, "file.credit before file.ready");
        return;
    }
    transfer->ready = true;
    /* Cumulative (File service); u32 steps cannot realistically reach the
     * top, but saturate rather than wrap. */
    const uint64_t bytes = osvc_get_u32(payload + 4);
    transfer->credit = transfer->credit > UINT64_MAX - bytes ?
                           UINT64_MAX : transfer->credit + bytes;
    s->file_drop.progress_ms = now_ms(s);
    report_credit(s, index);
}

static void handle_file_done(osvc_session_t *s, const uint8_t *payload,
                             size_t length)
{
    if (length < OSVC_FILE_DONE_FIXED_LEN ||
        payload[4] > OSVC_FILE_DONE_CANCELLED || payload[5] != 0U ||
        payload[6] != 0U || payload[7] != 0U ||
        (payload[4] != OSVC_FILE_DONE_OK &&
         length != OSVC_FILE_DONE_FIXED_LEN) ||
        !osvc_utf8_valid(payload + OSVC_FILE_DONE_FIXED_LEN,
                         length - OSVC_FILE_DONE_FIXED_LEN)) {
        fault_service(s, OSVC_SERVICE_FILE, "invalid file.done");
        return;
    }
    size_t index;
    if (!find_transfer(s, 0U, osvc_get_u32(payload), &index)) {
        return;
    }
    const osvc_file_transfer_t *transfer = &s->file_drop.transfers[index];
    if (payload[4] == OSVC_FILE_DONE_OK && transfer->sent != transfer->size) {
        /* OK means the sender wrote every byte, and the KVM has not sent
         * them all. */
        fault_service(s, OSVC_SERVICE_FILE,
                      "file.done OK before every byte was sent");
        return;
    }
    end_transfer(s, index, (osvc_file_status_t)payload[4],
                 (const char *)payload + OSVC_FILE_DONE_FIXED_LEN,
                 length - OSVC_FILE_DONE_FIXED_LEN, false);
}

static void handle_file(osvc_session_t *s, const uint8_t *payload,
                        size_t length)
{
    switch (s->kind) {
    case OSVC_FILE_CAPABILITIES:
        if (parse_flag_capabilities(s, OSVC_SERVICE_FILE, payload, length,
                                    OSVC_FILE_FLAG_RECEIVE,
                                    &s->file_receive) &&
            !s->file_receive) {
            /* A withdrawal fails every open transfer (File service). */
            end_transfers(s, OSVC_FILE_DONE_FAILED, true);
        }
        return;
    case OSVC_FILE_READY:
    case OSVC_FILE_CREDIT:
        handle_file_credit(s, payload, length);
        return;
    case OSVC_FILE_DONE:
        handle_file_done(s, payload, length);
        return;
    case OSVC_FILE_DROP:
    case OSVC_FILE_CHUNK:
    case OSVC_FILE_CANCEL:
        fault_service(s, OSVC_SERVICE_FILE,
                      "a KVM-to-sender file message from the sender");
        return;
    default:
        s->counters.messages_skipped++;
        return;
    }
}

static void handle_update_capabilities(osvc_session_t *s,
                                       const uint8_t *payload, size_t length)
{
    if (length < 4U || (osvc_get_u32(payload) & ~OSVC_UPDATE_FLAG_INSTALL) != 0U ||
        length - 4U > OSVC_UPDATE_PACKAGE_MAX ||
        !osvc_utf8_valid(payload + 4, length - 4U) ||
        ((osvc_get_u32(payload) != 0U) != (length > 4U))) {
        fault_service(s, OSVC_SERVICE_UPDATE, "invalid update.capabilities");
        return;
    }
    s->service_faulted[OSVC_SERVICE_UPDATE] = false;
    s->update_install = osvc_get_u32(payload) != 0U;
    memcpy(s->update_package, payload + 4, length - 4U);
    s->update_package[length - 4U] = '\0';
    if (!s->update_install) {
        end_update(s, OSVC_UPDATE_STATUS_FAILED,
                   "the sender withdrew the update service", true);
    }
    notify_state(s);
}

static void handle_update_credit(osvc_session_t *s, const uint8_t *payload,
                                 size_t length)
{
    const bool ready = s->kind == OSVC_UPDATE_READY;
    if (length != 8U) {
        fault_service(s, OSVC_SERVICE_UPDATE,
                      ready ? "invalid update.ready" : "invalid update.credit");
        return;
    }
    osvc_update_t *update = &s->update;
    if (!update_open(s, osvc_get_u32(payload))) {
        return; /* ended here while the message was in transit */
    }
    if (ready == update->ready) {
        fault_service(s, OSVC_SERVICE_UPDATE,
                      ready ? "a second update.ready" :
                              "update.credit before update.ready");
        return;
    }
    update->ready = true;
    const uint64_t bytes = osvc_get_u32(payload + 4);
    update->credit = update->credit > UINT64_MAX - bytes ?
                         UINT64_MAX : update->credit + bytes;
    update->progress_ms = now_ms(s);
}

static void handle_update_status(osvc_session_t *s, const uint8_t *payload,
                                 size_t length)
{
    if (length < 8U || payload[4] > OSVC_UPDATE_STATUS_CANCELLED ||
        (payload[5] | payload[6] | payload[7]) != 0U ||
        (payload[4] != OSVC_UPDATE_STATUS_FAILED && length != 8U) ||
        !osvc_utf8_valid(payload + 8, length - 8U)) {
        fault_service(s, OSVC_SERVICE_UPDATE, "invalid update.status");
        return;
    }
    if (!update_open(s, osvc_get_u32(payload))) {
        return;
    }
    if (payload[4] == OSVC_UPDATE_STATUS_INSTALLING &&
        s->update.sent != s->update.size) {
        fault_service(s, OSVC_SERVICE_UPDATE,
                      "update.status Installing before every byte was sent");
        return;
    }
    /* Cut on a character boundary: a continuation byte cannot start the
     * rest. */
    size_t kept = length - 8U;
    if (kept >= OSVC_UPDATE_MESSAGE_CAPACITY) {
        kept = OSVC_UPDATE_MESSAGE_CAPACITY - 1U;
        while (kept > 0U && (payload[8U + kept] & 0xC0U) == 0x80U) {
            kept--;
        }
    }
    /* What JSON escapes would grow the browser's event: shown as '?'. */
    char message[OSVC_UPDATE_MESSAGE_CAPACITY];
    for (size_t i = 0U; i < kept; ++i) {
        const uint8_t c = payload[8U + i];
        message[i] = c < 0x20U || c == '"' || c == '\\' ? '?' : (char)c;
    }
    message[kept] = '\0';
    end_update(s, payload[4], message, false);
    /* The sender exits to install the KVM's update; the next session
     * switches back, as after the KVM's own USB restart. */
    s->video_switch_back |= payload[4] == OSVC_UPDATE_STATUS_INSTALLING;
}

static void handle_update(osvc_session_t *s, const uint8_t *payload,
                          size_t length)
{
    switch (s->kind) {
    case OSVC_UPDATE_CAPABILITIES:
        handle_update_capabilities(s, payload, length);
        return;
    case OSVC_UPDATE_READY:
    case OSVC_UPDATE_CREDIT:
        handle_update_credit(s, payload, length);
        return;
    case OSVC_UPDATE_STATUS:
        handle_update_status(s, payload, length);
        return;
    case OSVC_UPDATE_BEGIN:
    case OSVC_UPDATE_CHUNK:
    case OSVC_UPDATE_CANCEL:
        fault_service(s, OSVC_SERVICE_UPDATE,
                      "a KVM-to-sender update message from the sender");
        return;
    default:
        s->counters.messages_skipped++;
        return;
    }
}

/* --- Service video -------------------------------------------------------- */

#define FRAME_KEY OSVC_VIDEO_FRAME_KEYFRAME
#define FRAME_SETS OSVC_VIDEO_FRAME_PARAMETER_SETS

static bool video_platform(const osvc_session_t *s)
{
    const osvc_platform_t *p = &s->platform;
    return p->display_present && p->display_restore && p->video_stream &&
           p->video_stopped && p->frame_reserve && p->frame_commit &&
           p->frame_cancel;
}

static bool video_settings_fit(const osvc_session_t *s)
{
    return s->settings.width <= s->video_max_width &&
           s->settings.height <= s->video_max_height;
}

static void set_source(osvc_session_t *s, osvc_video_source_t source)
{
    s->video_source = source;
    s->video_source_ms = now_ms(s);
    notify_state(s);
}

/* Takes a switch from the retry budget, or refuses it and blocks automatic
 * switches until the user turns video off and on. */
static bool recovery_allow(osvc_video_recovery_t *r, int64_t now)
{
    uint32_t kept = 0U;
    for (uint32_t i = 0U; i < r->count; ++i) {
        if (now - r->starts_ms[i] < OSVC_VIDEO_RETRY_WINDOW_MS) {
            r->starts_ms[kept++] = r->starts_ms[i];
        }
    }
    r->count = kept;
    r->blocked |= kept >= OSVC_VIDEO_RETRY_SWITCHES;
    if (!r->blocked) {
        r->starts_ms[r->count++] = now;
    }
    return !r->blocked;
}

/* Takes the current switch back out of the retry budget, once: its end was
 * no failure of the switch. */
static void video_refund_switch(osvc_session_t *s)
{
    osvc_video_recovery_t *r = &s->video_recovery;
    if (!s->video_switch_refunded && !r->blocked && r->count > 0U) {
        r->count--;
    }
    s->video_switch_refunded = true;
}

/* Gives the platform's keyframe request back: answered, or to be asked
 * again. */
static void give_back_keyframe(osvc_session_t *s, bool delivered)
{
    if (s->video_keyframe_generation != 0U) {
        s->platform.keyframe_done(s->platform.ctx,
                                  s->video_keyframe_generation, delivered);
        s->video_keyframe_generation = 0U;
    }
}

/* Takes the platform's keyframe request unless one is held; true while one
 * is held. */
static bool take_keyframe_request(osvc_session_t *s, int64_t now)
{
    if (s->video_keyframe_generation == 0U && s->platform.keyframe_wanted &&
        s->platform.keyframe_done) {
        s->video_keyframe_generation =
            s->platform.keyframe_wanted(s->platform.ctx);
        s->video_keyframe_taken_ms = now;
    }
    return s->video_keyframe_generation != 0U;
}

/* Frames stop; the platform's stream ends if the KVM owned it. */
static void video_stop_stream(osvc_session_t *s)
{
    frame_release(s);
    give_back_keyframe(s, false);
    s->video_stream_started = false;
    s->video_reset_pending = false;
    if (s->video_source == OSVC_SOURCE_SWITCHING_TO_GUEST ||
        s->video_source == OSVC_SOURCE_GUEST) {
        s->platform.video_stopped(s->platform.ctx);
    }
}

/* Returns to hardware capture now (Switch to hardware capture). */
static void video_restore(osvc_session_t *s)
{
    if (s->video_source != OSVC_SOURCE_SWITCHING_TO_GUEST &&
        s->video_source != OSVC_SOURCE_GUEST && !s->video_disable_pending) {
        return;
    }
    video_stop_stream(s);
    s->video_disable_pending = false;
    s->video_restoring = true;
    s->platform.display_restore(s->platform.ctx);
    set_source(s, OSVC_SOURCE_SWITCHING_TO_PHYSICAL);
}

/* The KVM stops service video: video.config with enabled 0, and its display
 * stays until the sender reports video disabled. */
static void video_disable(osvc_session_t *s)
{
    if (!s->video_sender_enabled) {
        video_restore(s);
        return;
    }
    video_stop_stream(s);
    s->video_disable_pending = true;
    set_source(s, OSVC_SOURCE_SWITCHING_TO_PHYSICAL);
    (void)send_video_config(s, false);
}

/* The EDID serial of the KVM display, not zero, which an EDID uses for "no
 * serial".
 *
 * For a sender that sets the display's mode it is the start of the KVM's key
 * with the height in its lowest 12 bits, the same at every presentation: the
 * target knows one monitor per KVM and size again, and the sender gives it
 * the settings' size. The height is in it because some desktops tell
 * monitors apart by name and serial only.
 *
 * For any other sender it is new at every presentation: a target adopts the
 * preferred timing of a monitor it has not seen, which is the settings'
 * size, and a mode that the user picked is kept for a monitor that never
 * returns. */
static bool video_display_serial(osvc_session_t *s, uint32_t *serial)
{
    if ((s->video_codec_flags & OSVC_VIDEO_FLAG_SETS_DISPLAY_MODE) != 0U) {
        uint8_t key[OSVC_PUBLIC_KEY_LEN];
        if (!s->platform.public_key(s->platform.ctx, key)) {
            return false;
        }
        *serial = (osvc_get_u32(key) & ~0xFFFU) | (s->settings.height & 0xFFFU);
        return true;
    }
    *serial = 0U;
    while (*serial == 0U) {
        s->platform.fill_random(s->platform.ctx, (uint8_t *)serial,
                                sizeof(*serial));
    }
    return true;
}

/* Presents the KVM display at the settings' size. */
static bool video_present(osvc_session_t *s)
{
    uint32_t serial;
    if (!video_display_serial(s, &serial)) {
        return false;
    }
    return s->platform.display_present(s->platform.ctx, s->settings.width,
                                       s->settings.height, serial);
}

/* Switch to service video: stop hardware capture and present the display;
 * video.config with enabled 1 follows once it is presented. */
static void video_start_switch(osvc_session_t *s)
{
    const char *refusal = VIDEO_BLOCKED;
    if (recovery_allow(&s->video_recovery, now_ms(s))) {
        s->video_switch_refunded = false;
        s->video_counters.switches++;
        /* Each switch starts at the static targets. */
        uint32_t kbps;
        uint32_t fps;
        video_targets(&s->settings, &kbps, &fps);
        video_rate_reset(s, kbps, now_ms(s));
        if (video_present(s)) {
            s->video_restoring = false;
            s->video_config_enabled = false;
            set_source(s, OSVC_SOURCE_SWITCHING_TO_GUEST);
            return;
        }
        s->video_counters.switch_failures++;
        refusal = VIDEO_NOT_PRESENTED;
    }
    snprintf(s->video_error, sizeof(s->video_error), "%s", refusal);
    if (s->video_answer_pending) {
        (void)send_video_config(s, false);
    }
    notify_state(s);
}

/* Acts on a registration or the user's wish, given the source. */
static void video_update(osvc_session_t *s)
{
    if (s->link != OSVC_LINK_OPEN || !s->video_registered) {
        s->video_switch_wanted = false;
        return;
    }
    s->video_switch_wanted &= s->settings.video_enabled && video_platform(s);
    switch (s->video_source) {
    case OSVC_SOURCE_PHYSICAL:
        if (s->video_switch_wanted) {
            s->video_switch_wanted = false;
            video_start_switch(s);
        } else if (s->video_answer_pending) {
            (void)send_video_config(s, false);
        }
        break;
    case OSVC_SOURCE_SWITCHING_TO_PHYSICAL:
        /* A wanted switch starts, and answers, once the display is back. */
        if (!s->video_switch_wanted && s->video_answer_pending) {
            (void)send_video_config(s, false);
        }
        break;
    default:
        /* Before the display is presented, the answer waits for it. */
        s->video_switch_wanted = false;
        if (s->video_answer_pending && s->video_config_enabled) {
            (void)send_video_config(s, true);
        }
        break;
    }
}

void osvc_session_display_status(osvc_session_t *s, bool presented,
                                 bool owned)
{
    switch (s->video_source) {
    case OSVC_SOURCE_SWITCHING_TO_PHYSICAL:
        if (s->video_restoring && !owned) {
            s->video_restoring = false;
            set_source(s, OSVC_SOURCE_PHYSICAL);
            video_update(s);
        }
        break;
    case OSVC_SOURCE_SWITCHING_TO_GUEST:
    case OSVC_SOURCE_GUEST:
        if (!owned) {
            fault_service(s, OSVC_SERVICE_VIDEO, VIDEO_NOT_PRESENTED);
        } else if (presented && !s->video_config_enabled) {
            (void)send_video_config(s, true);
        }
        break;
    default:
        break;
    }
}

void osvc_session_display_held(osvc_session_t *s, bool held)
{
    if (s->video_display_held && !held) {
        s->video_source_ms = now_ms(s);
    }
    s->video_display_held = held;
    if (held && s->video_answer_pending) {
        (void)send_video_config(s, false);
    }
}

static void handle_video_capabilities(osvc_session_t *s, const uint8_t *payload,
                                      size_t length)
{
    if (length != OSVC_VIDEO_CAPABILITIES_LEN) {
        fault_service(s, OSVC_SERVICE_VIDEO, "invalid video.capabilities length");
        return;
    }
    const uint32_t flags = osvc_get_u32(payload);
    const uint32_t width = osvc_get_u32(payload + 4);
    const uint32_t height = osvc_get_u32(payload + 8);
    const uint32_t fps = osvc_get_u32(payload + 12);
    const bool withdrawal = (flags | width | height | fps) == 0U;
    const uint32_t known = OSVC_VIDEO_CODEC_FLAG_H264 |
                           OSVC_VIDEO_CODEC_FLAG_H265 |
                           OSVC_VIDEO_FLAG_SETS_DISPLAY_MODE;
    if (!withdrawal &&
        ((flags & OSVC_VIDEO_CODEC_FLAG_H264) == 0U || (flags & ~known) != 0U ||
         width == 0U || width > OSVC_MAX_DIMENSION || height == 0U ||
         height > OSVC_MAX_DIMENSION || fps == 0U || fps > OSVC_MAX_FPS)) {
        fault_service(s, OSVC_SERVICE_VIDEO, "invalid video.capabilities");
        return;
    }
    if (withdrawal && s->video_sender_enabled) {
        fault_service(s, OSVC_SERVICE_VIDEO,
                      "video withdrawn without a disabled video.status");
        return;
    }
    s->service_faulted[OSVC_SERVICE_VIDEO] = false;
    s->video_error[0] = '\0';
    s->video_registered = !withdrawal;
    s->video_codec_flags = flags;
    s->video_max_width = width;
    s->video_max_height = height;
    s->video_max_fps = fps;
    s->video_answer_pending = !withdrawal;
    s->video_switch_wanted = !withdrawal;
    if (withdrawal) {
        video_restore(s);
    } else if (!video_settings_fit(s)) {
        fault_service(s, OSVC_SERVICE_VIDEO, VIDEO_TOO_LARGE);
        return;
    }
    video_update(s);
    notify_state(s);
}

/* Why an enabled video.status breaks the rules, or NULL. */
static const char *enabled_status_error(const osvc_session_t *s,
                                        const uint8_t *payload)
{
    const uint32_t width = osvc_get_u32(payload + 4);
    const uint32_t height = osvc_get_u32(payload + 8);
    const uint32_t native_fps = osvc_get_u32(payload + 12);
    if (width == 0U || width > OSVC_MAX_DIMENSION || height == 0U ||
        height > OSVC_MAX_DIMENSION || native_fps == 0U ||
        native_fps > OSVC_MAX_FPS) {
        return "an enabled video.status with an invalid size or rate";
    }
    if (payload[2] == OSVC_CODEC_H265 &&
        (s->video_codec_flags & OSVC_VIDEO_CODEC_FLAG_H265) == 0U) {
        return "video.status reports H.265, which the sender did not advertise";
    }
    if (width > s->video_max_width || height > s->video_max_height ||
        osvc_get_u32(payload + 24) > s->video_max_fps) {
        return "video.status exceeds the sender's video.capabilities";
    }
    if (!s->video_sender_enabled) {
        return "an enabled video.status without a switch to service video";
    }
    if (payload[2] != OSVC_CODEC_H264) {
        return "video.status reports H.265; the KVM asked for H.264";
    }
    return NULL;
}

/* A coded sequence of width x height at fps starts with the next frame. */
static void video_start_stream(osvc_session_t *s, uint32_t width,
                               uint32_t height, uint32_t fps, bool reset)
{
    give_back_keyframe(s, false);
    const size_t capacity =
        s->platform.video_stream(s->platform.ctx, width, height, fps);
    if (capacity == 0U) {
        char reason[64];
        snprintf(reason, sizeof(reason),
                 "the KVM cannot show %" PRIu32 "x%" PRIu32 " at %" PRIu32
                 " fps",
                 width, height, fps);
        fault_service(s, OSVC_SERVICE_VIDEO, reason);
        return;
    }
    /* SPS and PPS of another size never describe this stream. */
    if (reset || width != s->video_width || height != s->video_height) {
        s->parameter_sets_length = 0U;
    }
    s->video_width = width;
    s->video_height = height;
    s->video_fps = fps;
    s->video_frame_capacity = capacity;
    s->video_stream_started = true;
    s->video_pts_restart = true;
    s->video_sent_capture_valid = false;
    /* The new stream's first keyframe crosses before the targets change. */
    osvc_video_rate_hold(&s->video_rate, now_ms(s));
    video_interval_restart(s, now_ms(s));
    if (reset) {
        s->video_reset_pending = true;
        s->video_counters.stream_resets++;
    }
    notify_state(s);
}

static void handle_enabled_status(osvc_session_t *s, const uint8_t *payload)
{
    const char *error = enabled_status_error(s, payload);
    if (error != NULL) {
        fault_service(s, OSVC_SERVICE_VIDEO, error);
        return;
    }
    if (s->video_source == OSVC_SOURCE_SWITCHING_TO_GUEST &&
        s->video_config_enabled) {
        set_source(s, OSVC_SOURCE_GUEST);
    } else if (s->video_source != OSVC_SOURCE_GUEST) {
        /* Stale: the KVM has asked the sender to stop. */
        return;
    }
    const uint32_t width = osvc_get_u32(payload + 4);
    const uint32_t height = osvc_get_u32(payload + 8);
    const uint32_t native_fps = osvc_get_u32(payload + 12);
    const uint32_t maximum_fps = osvc_get_u32(payload + 24);
    s->video_sender_kbps = osvc_get_u32(payload + 20);
    const uint32_t fps =
        maximum_fps != 0U ? min_u32(native_fps, maximum_fps) : native_fps;
    if (payload[3] == 1U || !s->video_stream_started ||
        width != s->video_width || height != s->video_height ||
        fps != s->video_fps) {
        video_start_stream(s, width, height, fps, payload[3] == 1U);
    }
}

static void handle_disabled_status(osvc_session_t *s, const uint8_t *payload)
{
    s->video_sender_enabled = false;
    if (!s->video_disable_pending && s->video_source != OSVC_SOURCE_GUEST &&
        !(s->video_source == OSVC_SOURCE_SWITCHING_TO_GUEST &&
          s->video_config_enabled)) {
        return;
    }
    if (osvc_get_u32(payload + 28) == OSVC_VIDEO_ERROR_DISPLAY_INACTIVE) {
        /* An inactive display does not count against the budget. */
        video_refund_switch(s);
    }
    video_restore(s);
}

static void handle_video_status(osvc_session_t *s, const uint8_t *payload,
                                size_t length)
{
    if (!s->video_registered) {
        return;
    }
    if (length < OSVC_VIDEO_STATUS_MIN_LEN || payload[0] > 1U ||
        payload[1] > 1U || payload[2] > 1U || payload[3] > 1U ||
        osvc_get_u32(payload + 24) > OSVC_MAX_FPS ||
        !osvc_utf8_valid(payload + 32, length - 32U)) {
        fault_service(s, OSVC_SERVICE_VIDEO, "invalid video.status");
    } else if (payload[0] == 1U) {
        handle_enabled_status(s, payload);
    } else {
        handle_disabled_status(s, payload);
    }
}

static void handle_video(osvc_session_t *s, const uint8_t *payload,
                         size_t length)
{
    switch (s->kind) {
    case OSVC_VIDEO_CAPABILITIES:
        handle_video_capabilities(s, payload, length);
        return;
    case OSVC_VIDEO_STATUS:
        handle_video_status(s, payload, length);
        return;
    case OSVC_VIDEO_CONFIG:
    case OSVC_VIDEO_REQUEST_KEYFRAME:
        fault_service(s, OSVC_SERVICE_VIDEO,
                      "a KVM-to-sender video message from the sender");
        return;
    default:
        s->counters.messages_skipped++;
        return;
    }
}

/* The user's video settings changed (Switch to service video). */
static void video_apply_settings(osvc_session_t *s,
                                 const osvc_settings_t *previous)
{
    const osvc_settings_t *now = &s->settings;
    const bool resized =
        previous->width != now->width || previous->height != now->height;
    if (!s->video_registered ||
        (previous->video_enabled == now->video_enabled && !resized &&
         previous->layout == now->layout &&
         previous->target_kbps == now->target_kbps &&
         previous->maximum_fps == now->maximum_fps)) {
        return;
    }
    if (!video_settings_fit(s)) {
        fault_service(s, OSVC_SERVICE_VIDEO, VIDEO_TOO_LARGE);
        return;
    }
    /* The adapted targets belong to the previous settings. */
    s->video_rate.ceiling_kbps = 0U;
    const bool showing = s->video_source == OSVC_SOURCE_SWITCHING_TO_GUEST ||
                         s->video_source == OSVC_SOURCE_GUEST;
    if (!showing) {
        s->video_switch_wanted = now->video_enabled;
        if (!now->video_enabled) {
            (void)send_video_config(s, false);
        }
        video_update(s);
    } else if (!now->video_enabled) {
        video_disable(s);
    } else if (resized) {
        /* The sender hears of the new size once the display is up. */
        s->video_config_enabled = false;
        if (!video_present(s)) {
            fault_service(s, OSVC_SERVICE_VIDEO, VIDEO_NOT_PRESENTED);
        }
    } else if (s->video_config_enabled) {
        (void)send_video_config(s, true);
    }
}

/* --- Video frames ------------------------------------------------------- */

static bool video_accepting(const osvc_session_t *s)
{
    return s->link == OSVC_LINK_OPEN && s->video_registered &&
           s->video_source == OSVC_SOURCE_GUEST && s->video_stream_started;
}

/* Leaves the frame being read: its slot goes back and its rest is skipped. */
static void frame_release(osvc_session_t *s)
{
    if (s->frame_slot != NULL) {
        s->platform.frame_cancel(s->platform.ctx, s->frame_fill);
        s->frame_slot = NULL;
    }
    s->frame_waiting = false;
    if (s->parse == OSVC_PARSE_FRAME_HEAD || s->parse == OSVC_PARSE_FRAME_WAIT ||
        s->parse == OSVC_PARSE_FRAME_DATA || s->parse == OSVC_PARSE_FRAME_SETS) {
        s->parse = OSVC_PARSE_SKIP;
    }
}

/* A frame is lost before the platform got it: the viewer needs a keyframe. */
static void frame_drop(osvc_session_t *s)
{
    s->video_counters.frames_dropped++;
    frame_release(s);
    if (s->platform.video_gap) {
        s->platform.video_gap(s->platform.ctx);
    }
}

/* A video.frame envelope: true when the frame is read into a slot, false
 * when it is skipped. */
static bool begin_frame(osvc_session_t *s)
{
    if (!video_accepting(s)) {
        s->video_counters.frames_discarded++;
        s->video_sent_capture_valid = false;
        return false;
    }
    if (s->payload_size <= OSVC_VIDEO_FRAME_HEADER_LEN) {
        fault_service(s, OSVC_SERVICE_VIDEO,
                      "a video.frame without an access unit");
        return false;
    }
    s->frame_arrival_us = s->platform.now_us ?
                              s->platform.now_us(s->platform.ctx) :
                              now_ms(s) * 1000;
    s->frame_head_fill = 0U;
    s->parse = OSVC_PARSE_FRAME_HEAD;
    return true;
}

/* Why a frame's head breaks the rules (video.frame, Keyframes and stream
 * boundaries), or NULL. */
static const char *frame_head_error(const osvc_session_t *s)
{
    if ((s->frame_flags & ~(uint32_t)(FRAME_KEY | FRAME_SETS)) != 0U) {
        return "a video.frame with unknown flags";
    }
    if (s->video_reset_pending && s->frame_flags != (FRAME_KEY | FRAME_SETS)) {
        return "the first frame of a stream is not a keyframe with "
               "parameter sets";
    }
    return NULL;
}

/* At the frame's head, which every frame the sender sent passes, also one
 * the KVM then drops; frame_pts() sees only the frames that got a slot. */
static void count_sender_skips(osvc_session_t *s)
{
    const uint64_t frame_us = 1000000U / s->video_fps;
    const uint64_t delta = s->frame_capture_us - s->video_sent_capture_us;
    if (s->video_sent_capture_valid &&
        s->frame_capture_us > s->video_sent_capture_us && delta <= 1000000U) {
        const uint64_t periods = (delta + frame_us / 2U) / frame_us;
        if (periods > 1U) {
            s->video_counters.sender_skipped_frames += (uint32_t)(periods - 1U);
        }
    }
    s->video_sent_capture_us = s->frame_capture_us;
    s->video_sent_capture_valid = true;
}

static void frame_head_done(osvc_session_t *s)
{
    s->frame_capture_us = osvc_get_u64(s->frame_head);
    s->frame_flags = osvc_get_u32(s->frame_head + 8);
    s->frame_unit_length = s->payload_size - OSVC_VIDEO_FRAME_HEADER_LEN;
    const char *error = frame_head_error(s);
    if (error != NULL) {
        fault_service(s, OSVC_SERVICE_VIDEO, error);
        return;
    }
    count_sender_skips(s);
    /* A keyframe without its SPS and PPS gets the stream's. */
    s->frame_prefix = s->frame_flags == FRAME_KEY ? s->parameter_sets_length : 0U;
    /* A keyframe that cannot fit a slot would come back on every request. */
    if ((s->frame_flags & FRAME_KEY) != 0U &&
        s->frame_prefix + s->frame_unit_length > s->video_frame_capacity) {
        char reason[48];
        snprintf(reason, sizeof(reason), "a keyframe above %zu bytes",
                 s->video_frame_capacity);
        fault_service(s, OSVC_SERVICE_VIDEO, reason);
        return;
    }
    /* The sender started the stream as the rule says, also when no viewer
     * takes this frame or it is dropped. */
    s->video_reset_pending = false;
    s->frame_fill = 0U;
    s->parse = OSVC_PARSE_FRAME_WAIT;
}

/* How long the frame has waited for a slot in the current network
 * interval. */
static uint32_t interval_wait(const osvc_session_t *s, int64_t now)
{
    const int64_t from = s->frame_wait_ms > s->video_interval_ms ?
                             s->frame_wait_ms : s->video_interval_ms;
    return s->frame_waiting && now > from ? (uint32_t)(now - from) : 0U;
}

static void note_slot_wait(osvc_session_t *s, int64_t now)
{
    const uint32_t waited =
        s->frame_waiting ? (uint32_t)(now - s->frame_wait_ms) : 0U;
    s->video_interval_wait_ms += interval_wait(s, now);
    s->video_counters.slot_wait_ms += waited;
    if (waited > s->video_counters.slot_wait_max_ms) {
        s->video_counters.slot_wait_max_ms = waited;
    }
    s->frame_waiting = false;
}

/* Reserves the frame's slot. False while the frame still waits for one. */
static bool frame_try_reserve(osvc_session_t *s)
{
    bool busy = false;
    const bool fits = s->frame_unit_length <= s->video_frame_capacity;
    s->frame_slot = fits ? s->platform.frame_reserve(
                               s->platform.ctx,
                               s->frame_prefix + s->frame_unit_length, &busy) :
                           NULL;
    const int64_t now = now_ms(s);
    if (!fits) {
        frame_drop(s);
    } else if (s->frame_slot != NULL) {
        note_slot_wait(s, now);
        memcpy(s->frame_slot, s->parameter_sets, s->frame_prefix);
        s->frame_fill = s->frame_prefix;
        s->parse = OSVC_PARSE_FRAME_DATA;
    } else if (!busy) {
        /* No viewer takes video. */
        s->video_counters.frames_discarded++;
        frame_release(s);
    } else if (!s->frame_waiting) {
        s->frame_waiting = true;
        s->frame_wait_ms = now;
        s->video_counters.slot_waits++;
        return false;
    } else if (now - s->frame_wait_ms < OSVC_VIDEO_SLOT_WAIT_MS) {
        return false;
    } else {
        note_slot_wait(s, now);
        frame_drop(s);
    }
    if (s->parse == OSVC_PARSE_SKIP && (s->frame_flags & FRAME_SETS) != 0U) {
        /* Later keyframes without SPS and PPS still need this frame's. */
        s->parse = OSVC_PARSE_FRAME_SETS;
    }
    return true;
}

/* The start of the next Annex-B start code at or after from (its leading
 * zero byte included), with the NAL unit after it in *nal; length when there
 * is none. */
static size_t start_code_at(const uint8_t *data, size_t length, size_t from,
                            size_t *nal)
{
    for (size_t i = from; i + 3U <= length; ++i) {
        if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 1U) {
            *nal = i + 3U;
            return i > from && data[i - 1U] == 0U ? i - 1U : i;
        }
    }
    *nal = length;
    return length;
}

/* The SPS and PPS ahead of the first slice of an H.264 access unit, each
 * behind a 4-byte start code: their size, copied to out unless it is NULL.
 * *both tells whether there are both and a slice follows them, so neither
 * is cut off when only the start of the unit was read. */
static size_t parameter_sets(const uint8_t *unit, size_t length, uint8_t *out,
                             bool *both)
{
    static const uint8_t start[4] = {0U, 0U, 0U, 1U};
    bool sps = false;
    bool pps = false;
    size_t size = 0U;
    size_t nal = 0U;
    (void)start_code_at(unit, length, 0U, &nal);
    while (nal < length && ((unit[nal] & 0x1fU) < 1U || (unit[nal] & 0x1fU) > 5U)) {
        const uint8_t type = unit[nal] & 0x1fU;
        size_t next = 0U;
        const size_t end = start_code_at(unit, length, nal + 1U, &next);
        if (type == 7U || type == 8U) {
            if (out != NULL) {
                memcpy(out + size, start, sizeof(start));
                memcpy(out + size + sizeof(start), unit + nal, end - nal);
            }
            size += sizeof(start) + end - nal;
            sps |= type == 7U;
            pps |= type == 8U;
        }
        nal = next;
    }
    *both = sps && pps && nal < length;
    return size;
}

/* Keeps the unit's SPS and PPS for later keyframes; true when it has both. */
static bool keep_parameter_sets(osvc_session_t *s, const uint8_t *unit,
                                size_t length)
{
    bool both = false;
    const size_t size = parameter_sets(unit, length, NULL, &both);
    if (both) {
        s->parameter_sets_length =
            size <= sizeof(s->parameter_sets) ?
                parameter_sets(unit, length, s->parameter_sets, &both) : 0U;
    }
    return both;
}

/* Why a complete access unit breaks the rules, or NULL. */
static const char *frame_unit_error(osvc_session_t *s, const uint8_t *unit)
{
    const size_t length = s->frame_unit_length;
    if (osvc_annexb_is_keyframe(OSVC_CODEC_H264, unit, length) !=
        ((s->frame_flags & FRAME_KEY) != 0U)) {
        return "the video.frame keyframe flag does not match its access unit";
    }
    if ((s->frame_flags & FRAME_SETS) != 0U &&
        !keep_parameter_sets(s, unit, length)) {
        return "a video.frame flagged with parameter sets has no SPS and PPS";
    }
    return NULL;
}

/* The frame's presentation time: the sender's capture spacing when it is 1
 * us to 1 s, else one frame at the stream's rate. A stream starts at the
 * KVM's clock, never before the frame that came last. */
static uint32_t frame_pts(osvc_session_t *s)
{
    const uint64_t frame_us = 1000000U / s->video_fps;
    const uint64_t delta = s->frame_capture_us - s->video_last_capture_us;
    if (s->video_pts_restart) {
        const uint64_t anchor = (uint64_t)s->frame_arrival_us;
        const uint64_t next = s->video_pts_us + frame_us;
        s->video_pts_us = s->video_pts_valid && next > anchor ? next : anchor;
    } else {
        s->video_pts_us += delta >= 1U && delta <= 1000000U ? delta : frame_us;
    }
    s->video_pts_restart = false;
    s->video_pts_valid = true;
    s->video_last_capture_us = s->frame_capture_us;
    return (uint32_t)(s->video_pts_us * 9U / 100U);
}

static void count_frame(osvc_video_counters_t *c, const osvc_video_frame_t *frame)
{
    c->frames++;
    c->bytes += frame->length;
    c->keyframes += frame->keyframe ? 1U : 0U;
    c->parameter_sets_added += frame->parameter_sets_added ? 1U : 0U;
    if (frame->keyframe && frame->length > c->largest_keyframe) {
        c->largest_keyframe = (uint32_t)frame->length;
    }
}

/* The access unit is complete in its slot: check it and hand it over. */
static void frame_commit(osvc_session_t *s)
{
    const char *error = frame_unit_error(s, s->frame_slot + s->frame_prefix);
    if (error != NULL) {
        frame_release(s);
        fault_service(s, OSVC_SERVICE_VIDEO, error);
        return;
    }
    const int64_t now = now_ms(s);
    const bool keyframe = (s->frame_flags & FRAME_KEY) != 0U;
    const osvc_video_frame_t frame = {
        .length = s->frame_fill,
        .pts_90k = frame_pts(s),
        .arrival_us = s->frame_arrival_us,
        .received_us = s->platform.now_us ?
                           s->platform.now_us(s->platform.ctx) : now * 1000,
        .width = s->video_width,
        .height = s->video_height,
        .fps = s->video_fps,
        /* A keyframe answers the platform's request, also one not yet
         * asked of the sender. */
        .keyframe_generation =
            keyframe && take_keyframe_request(s, now) ?
                s->video_keyframe_generation : 0U,
        .keyframe = keyframe,
        .parameter_sets_added = s->frame_prefix != 0U,
    };
    s->frame_slot = NULL;
    s->last_message_ms = now;
    s->video_interval_frames++;
    count_frame(&s->video_counters, &frame);
    const bool delivered = s->platform.frame_commit(s->platform.ctx, &frame);
    if (frame.keyframe_generation != 0U) {
        give_back_keyframe(s, delivered);
    }
}

/* Asks the sender for a keyframe, at most once per interval. A busy IN
 * endpoint skips the request; it goes again after the interval. */
static void request_keyframe(osvc_session_t *s, int64_t now)
{
    if (s->video_keyframe_requested &&
        now - s->video_keyframe_request_ms < OSVC_VIDEO_KEYFRAME_INTERVAL_MS) {
        return;
    }
    s->video_keyframe_requested = true;
    s->video_keyframe_request_ms = now;
    if (write_message(s, OSVC_SERVICE_VIDEO, OSVC_VIDEO_REQUEST_KEYFRAME, NULL,
                      0U, NULL, 0U)) {
        s->video_counters.keyframe_requests++;
    }
}

/* What the KVM carries, from an interval in which frames waited for a
 * slot: under what the transport sent and under the target, because a
 * sender that sends more than its target would otherwise never be cut
 * below it. When the sender has shown how many frames it delivers at this
 * target while none waits long (video_free_mfps), and the KVM now took
 * fewer, it is that share: the KVM holds the USB read while a frame
 * waits, so the rate that was sent belongs to the lower frame rate.
 * Without that evidence the frames are not counted, because a sender can
 * also deliver fewer frames by itself. */
static uint32_t carried_kbps(const osvc_session_t *s, uint32_t sent_kbps,
                             uint32_t mfps)
{
    const uint64_t under = (uint64_t)min_u32(sent_kbps, s->video_rate.kbps) *
                           OSVC_VIDEO_PRESSURE_SENT_PERCENT / 100U;
    return (uint32_t)(mfps < s->video_free_mfps ?
                          under * mfps / s->video_free_mfps : under);
}

/* The estimate for an interval that just ended: the receiver feedback's
 * (with a manual video quality, the static target), or what the KVM
 * carries when frames waited for a slot much of the time, interval after
 * interval. A feedback target that did not just fall cuts nothing: after
 * one burst of loss it stays low for seconds while it recovers, and is no
 * new evidence. */
static uint32_t video_estimate(osvc_session_t *s,
                               const osvc_video_network_t *network,
                               int64_t now)
{
    const uint64_t waited = s->video_interval_wait_ms + interval_wait(s, now);
    const int64_t length = now - s->video_interval_ms;
    const uint32_t mfps =
        length > 0 ? (uint32_t)((uint64_t)s->video_interval_frames * 1000000U /
                                (uint64_t)length) : 0U;
    s->video_interval_ms = now;
    s->video_interval_wait_ms = 0U;
    s->video_interval_frames = 0U;
    const bool pressed =
        length > 0 &&
        waited * 1000U >= (uint64_t)OSVC_VIDEO_PRESSURE_PERMILLE * (uint64_t)length;
    s->video_pressure_intervals = pressed ? s->video_pressure_intervals + 1U : 0U;
    if (!pressed && mfps != 0U) {
        s->video_free_mfps = mfps;
    }
    const uint32_t estimate =
        !network->adaptive ? s->video_rate.ceiling_kbps :
        network->cut       ? network->estimate_kbps :
                             max_u32(network->estimate_kbps, s->video_rate.kbps);
    return s->video_pressure_intervals < OSVC_VIDEO_PRESSURE_INTERVALS ?
               estimate :
               min_u32(estimate, carried_kbps(s, network->sent_kbps, mfps));
}

/* Adapts the targets (os_services_video_rate.h): to the receiver feedback
 * with video quality Auto, and to the KVM's own slot waits with any video
 * quality. A manual video quality is the upper limit, not a promise: the
 * KVM still lowers the rate when it cannot send the frames. A change
 * between Auto and a manual quality puts the static targets back at once.
 * A fixed bitrate in the settings is never adapted. */
static void video_adapt(osvc_session_t *s, int64_t now)
{
    uint32_t ceiling;
    uint32_t fps;
    video_targets(&s->settings, &ceiling, &fps);
    const bool adapts =
        s->platform.video_network != NULL && s->settings.target_kbps == 0U;
    osvc_video_network_t network = {0};
    if (adapts) {
        s->platform.video_network(s->platform.ctx, ceiling, &network);
    }
    osvc_video_rate_t *rate = &s->video_rate;
    const bool adapted = rate->ceiling_kbps == ceiling &&
                         (rate->kbps != ceiling || rate->low_fps);
    const bool quality_changed = network.adaptive != s->video_rate_follows;
    s->video_rate_follows = network.adaptive;
    if (!adapts || rate->ceiling_kbps != ceiling ||
        (quality_changed && adapted)) {
        video_rate_reset(s, ceiling, now);
        if (adapted) {
            (void)send_video_config(s, true);
        }
        return;
    }
    if (network.interval &&
        osvc_video_rate_step(rate, video_estimate(s, &network, now), now)) {
        /* What the sender delivers at the new target is not known yet. */
        s->video_free_mfps = 0U;
        (void)send_video_config(s, true);
    }
}

static void video_tick(osvc_session_t *s, int64_t now)
{
    const int64_t elapsed = now - s->video_source_ms;
    if (s->video_source == OSVC_SOURCE_GUEST &&
        elapsed >= OSVC_VIDEO_RETRY_SHOWN_MS) {
        video_refund_switch(s);
    }
    if (s->video_source == OSVC_SOURCE_SWITCHING_TO_GUEST &&
        !s->video_display_held && elapsed >= OSVC_VIDEO_SWITCH_TIMEOUT_MS) {
        s->video_counters.switch_failures++;
        snprintf(s->video_error, sizeof(s->video_error), "%s", VIDEO_NO_STATUS);
        video_restore(s);
        if (s->video_registered) {
            (void)send_video_config(s, false);
        }
    } else if (s->video_disable_pending &&
               elapsed >= OSVC_VIDEO_DISABLE_TIMEOUT_MS) {
        video_restore(s);
    } else if (video_accepting(s)) {
        /* A request not answered in time is given back, then taken and
         * asked again. */
        if (s->video_keyframe_generation != 0U &&
            now - s->video_keyframe_taken_ms >= OSVC_VIDEO_KEYFRAME_INTERVAL_MS) {
            give_back_keyframe(s, false);
        }
        if (take_keyframe_request(s, now)) {
            request_keyframe(s, now);
        }
        if (s->video_config_enabled) {
            video_adapt(s, now);
        }
    }
}

bool osvc_session_frame_waiting(const osvc_session_t *s)
{
    return s->parse == OSVC_PARSE_FRAME_WAIT;
}

const char *osvc_video_source_name(osvc_video_source_t source)
{
    static const char *const names[] = {
        "physical", "switching_to_guest", "guest", "switching_to_physical",
    };
    return (size_t)source < sizeof(names) / sizeof(names[0]) ? names[source] :
                                                                 "physical";
}

static void dispatch(osvc_session_t *s, const uint8_t *payload, size_t length)
{
    s->counters.messages_in++;
    s->last_message_ms = now_ms(s);
    /* A sender that its user did not approve closes the session right
     * after the handshake; anything else means the KVM is approved. */
    if (s->session_messages++ == 0U && s->approval_pending &&
        !(s->service == OSVC_SERVICE_SESSION &&
          s->kind == OSVC_SESSION_CLOSE)) {
        s->approval_pending = false;
        notify_state(s);
    }
    if (s->service != OSVC_SERVICE_SESSION && s->service < OSVC_SERVICE_COUNT &&
        s->service_faulted[s->service]) {
        /* A faulted service is ignored until it registers again. */
        const bool capabilities = s->kind == 1U;
        if (!capabilities) {
            return;
        }
    }
    switch (s->service) {
    case OSVC_SERVICE_SESSION:
        handle_session(s, payload, length);
        break;
    case OSVC_SERVICE_VIDEO:
        handle_video(s, payload, length);
        break;
    case OSVC_SERVICE_CLIPBOARD:
        handle_clipboard(s, payload, length);
        break;
    case OSVC_SERVICE_TERMINAL:
        handle_terminal(s, payload, length);
        break;
    case OSVC_SERVICE_FILE:
        handle_file(s, payload, length);
        break;
    case OSVC_SERVICE_UPDATE:
        handle_update(s, payload, length);
        break;
    default:
        s->counters.messages_skipped++;
        break;
    }
}

/* Whether this firmware keeps the payload of a sender message: the known
 * sender-to-KVM kinds. Everything else is skipped by its envelope. */
static bool keeps_payload(uint16_t service, uint16_t kind)
{
    switch (service) {
    case OSVC_SERVICE_SESSION:
        return kind >= OSVC_SESSION_HELLO && kind <= OSVC_SESSION_INFO;
    case OSVC_SERVICE_VIDEO:
        return kind == OSVC_VIDEO_CAPABILITIES || kind == OSVC_VIDEO_STATUS ||
               kind == OSVC_VIDEO_CONFIG || kind == OSVC_VIDEO_REQUEST_KEYFRAME;
    case OSVC_SERVICE_CLIPBOARD:
        return (kind >= OSVC_CLIPBOARD_CAPABILITIES &&
                kind <= OSVC_CLIPBOARD_STATUS) ||
               kind == OSVC_CLIPBOARD_CONFIG;
    case OSVC_SERVICE_TERMINAL:
        return kind >= OSVC_TERMINAL_CAPABILITIES &&
               kind <= OSVC_TERMINAL_CREDIT;
    case OSVC_SERVICE_FILE:
        return kind >= OSVC_FILE_CAPABILITIES && kind <= OSVC_FILE_CANCEL;
    case OSVC_SERVICE_UPDATE:
        return kind >= OSVC_UPDATE_CAPABILITIES && kind <= OSVC_UPDATE_CANCEL;
    default:
        return false;
    }
}

/* The skipped message is read past. */
static void skip_done(osvc_session_t *s)
{
    s->parse = OSVC_PARSE_HEADER;
    s->counters.messages_skipped++;
    s->last_message_ms = now_ms(s);
}

/* A complete envelope: decide where its payload goes. */
static void begin_message(osvc_session_t *s)
{
    s->service = osvc_get_u16(s->header);
    s->kind = osvc_get_u16(s->header + 2);
    s->payload_size = osvc_get_u32(s->header + 4);
    s->payload_fill = 0U;
    s->header_fill = 0U;
    if (s->payload_size > osvc_payload_limit(s->service, s->kind)) {
        end_session(s, "a payload_size above its limit");
        return;
    }
    if (s->service == OSVC_SERVICE_VIDEO && s->kind == OSVC_VIDEO_FRAME &&
        begin_frame(s)) {
        return;
    }
    if (s->parse == OSVC_PARSE_STOPPED) {
        /* The frame faulted video, and the reply could not be written. */
        return;
    }
    if (keeps_payload(s->service, s->kind) &&
        s->payload_size <= s->payload_capacity) {
        s->parse = OSVC_PARSE_PAYLOAD;
    } else {
        /* Unknown kinds, and video.frame while service video is not shown. */
        s->parse = OSVC_PARSE_SKIP;
    }
    if (s->payload_size == 0U) {
        if (s->parse == OSVC_PARSE_PAYLOAD) {
            s->parse = OSVC_PARSE_HEADER;
            dispatch(s, s->payload, 0U);
        } else {
            skip_done(s);
        }
    }
}

/* Reads a frame no slot takes into the payload buffer, as far as it holds,
 * and keeps its SPS and PPS; the rest is skipped. Returns the bytes used. */
static size_t read_frame_sets(osvc_session_t *s, const uint8_t *data,
                              size_t length)
{
    const size_t fill = s->payload_fill - OSVC_VIDEO_FRAME_HEADER_LEN;
    const size_t size = min_size(s->frame_unit_length, s->payload_capacity);
    const size_t take = min_size(size - fill, length);
    memcpy(s->payload + fill, data, take);
    s->payload_fill += (uint32_t)take;
    s->counters.bytes_discarded += take;
    if (fill + take == size) {
        (void)keep_parameter_sets(s, s->payload, size);
        s->parse = OSVC_PARSE_SKIP;
        if (s->payload_fill == s->payload_size) {
            skip_done(s);
        }
    }
    return take;
}

static bool hello_at(const osvc_session_t *s, const uint8_t *p)
{
    return memcmp(p, s_hello_envelope, OSVC_ENVELOPE_LEN) == 0 &&
           memcmp(p + HELLO_TOKEN_OFFSET, s->token, OSVC_TOKEN_LEN) == 0;
}

/*
 * Searches for the complete session.hello with the RESET token
 * (Byte stream). Returns the bytes of data consumed: up to the end of the
 * hello when found, else all of them.
 */
static size_t search_hello(osvc_session_t *s, const uint8_t *data,
                           size_t length)
{
    enum { TAIL = OSVC_ENVELOPE_LEN + OSVC_HELLO_LEN - 1U };
    /* A hello that starts in the kept tail and ends in data. */
    if (s->search_tail_fill > 0U) {
        uint8_t joined[2U * TAIL];
        const size_t head = length < TAIL ? length : TAIL;
        memcpy(joined, s->search_tail, s->search_tail_fill);
        memcpy(joined + s->search_tail_fill, data, head);
        const size_t joined_length = s->search_tail_fill + head;
        for (size_t p = 0U; p < s->search_tail_fill &&
                            p + HELLO_MESSAGE_LEN <= joined_length;
             ++p) {
            if (hello_at(s, joined + p)) {
                uint8_t hello[OSVC_HELLO_LEN];
                memcpy(hello, joined + p + OSVC_ENVELOPE_LEN, OSVC_HELLO_LEN);
                const size_t consumed = p + HELLO_MESSAGE_LEN - s->search_tail_fill;
                s->counters.bytes_discarded += p;
                s->search_tail_fill = 0U;
                s->parse = OSVC_PARSE_HEADER;
                handle_hello(s, hello);
                return consumed;
            }
        }
    }
    for (size_t p = 0U; p + HELLO_MESSAGE_LEN <= length; ++p) {
        /* The kind byte, then the full check. */
        if (data[p + 2U] == OSVC_SESSION_HELLO && hello_at(s, data + p)) {
            s->counters.bytes_discarded += s->search_tail_fill + p;
            s->search_tail_fill = 0U;
            s->parse = OSVC_PARSE_HEADER;
            uint8_t hello[OSVC_HELLO_LEN];
            memcpy(hello, data + p + OSVC_ENVELOPE_LEN, OSVC_HELLO_LEN);
            handle_hello(s, hello);
            return p + HELLO_MESSAGE_LEN;
        }
    }
    /* Not found: keep the last TAIL bytes of tail + data. */
    s->searched += length;
    if (length >= TAIL) {
        s->counters.bytes_discarded += s->search_tail_fill + length - TAIL;
        memcpy(s->search_tail, data + length - TAIL, TAIL);
        s->search_tail_fill = TAIL;
    } else {
        const size_t total = s->search_tail_fill + length;
        if (total > TAIL) {
            const size_t drop = total - TAIL;
            memmove(s->search_tail, s->search_tail + drop,
                    s->search_tail_fill - drop);
            s->search_tail_fill -= drop;
            s->counters.bytes_discarded += drop;
        }
        memcpy(s->search_tail + s->search_tail_fill, data, length);
        s->search_tail_fill += length;
    }
    if (s->searched > OSVC_HELLO_SEARCH_LIMIT) {
        end_session(s, "no session.hello in 8 MiB of input");
    }
    return length;
}

size_t osvc_session_receive(osvc_session_t *s, const uint8_t *data,
                            size_t length)
{
    if (s->parse == OSVC_PARSE_HELD && !deliver_held(s)) {
        return 0U;
    }
    size_t used = 0U;
    while (used < length && s->parse != OSVC_PARSE_HELD) {
        if (s->link != OSVC_LINK_ADMITTED && s->link != OSVC_LINK_OPEN) {
            break;
        }
        const uint8_t *p = data + used;
        const size_t left = length - used;
        switch (s->parse) {
        case OSVC_PARSE_SEARCH:
            used += search_hello(s, p, left);
            break;
        case OSVC_PARSE_HEADER: {
            size_t take = OSVC_ENVELOPE_LEN - s->header_fill;
            take = take < left ? take : left;
            memcpy(s->header + s->header_fill, p, take);
            s->header_fill += take;
            used += take;
            if (s->header_fill == OSVC_ENVELOPE_LEN) {
                begin_message(s);
            }
            break;
        }
        case OSVC_PARSE_PAYLOAD: {
            size_t take = s->payload_size - s->payload_fill;
            take = take < left ? take : left;
            memcpy(s->payload + s->payload_fill, p, take);
            s->payload_fill += (uint32_t)take;
            used += take;
            if (s->payload_fill == s->payload_size) {
                s->parse = OSVC_PARSE_HEADER;
                dispatch(s, s->payload, s->payload_size);
            }
            break;
        }
        case OSVC_PARSE_SKIP: {
            size_t take = s->payload_size - s->payload_fill;
            take = take < left ? take : left;
            s->payload_fill += (uint32_t)take;
            s->counters.bytes_discarded += take;
            used += take;
            if (s->payload_fill == s->payload_size) {
                skip_done(s);
            }
            break;
        }
        case OSVC_PARSE_FRAME_HEAD: {
            const size_t take = min_size(
                OSVC_VIDEO_FRAME_HEADER_LEN - s->frame_head_fill, left);
            memcpy(s->frame_head + s->frame_head_fill, p, take);
            s->frame_head_fill += take;
            s->payload_fill += (uint32_t)take;
            used += take;
            if (s->frame_head_fill == OSVC_VIDEO_FRAME_HEADER_LEN) {
                frame_head_done(s);
            }
            break;
        }
        case OSVC_PARSE_FRAME_WAIT:
            if (!frame_try_reserve(s)) {
                /* The caller keeps the rest of the read. */
                length = used;
            }
            break;
        case OSVC_PARSE_FRAME_DATA: {
            const size_t take = min_size(s->payload_size - s->payload_fill, left);
            memcpy(s->frame_slot + s->frame_fill, p, take);
            s->frame_fill += take;
            s->payload_fill += (uint32_t)take;
            used += take;
            if (s->payload_fill == s->payload_size) {
                s->parse = OSVC_PARSE_HEADER;
                frame_commit(s);
            }
            break;
        }
        case OSVC_PARSE_FRAME_SETS:
            used += read_frame_sets(s, p, left);
            break;
        case OSVC_PARSE_STOPPED:
        default:
            return used;
        }
    }
    s->counters.bytes_in += used;
    return used;
}

bool osvc_session_holding(const osvc_session_t *s)
{
    return s->parse == OSVC_PARSE_HELD;
}

/* The protocol has no all-zero token, so a sender reads any change of the
 * token as a new owner. */
static void fill_token(osvc_session_t *s, uint8_t token[OSVC_TOKEN_LEN])
{
    do {
        s->platform.fill_random(s->platform.ctx, token, OSVC_TOKEN_LEN);
    } while (memcmp(token, (const uint8_t[OSVC_TOKEN_LEN]){0},
                    OSVC_TOKEN_LEN) == 0);
}

void osvc_session_init(osvc_session_t *s, const osvc_platform_t *platform,
                       const osvc_settings_t *settings, uint8_t *payload_buffer,
                       size_t payload_capacity)
{
    memset(s, 0, sizeof(*s));
    s->platform = *platform;
    s->settings = *settings;
    s->payload = payload_buffer;
    s->payload_capacity = payload_capacity;
    s->link = OSVC_LINK_IDLE;
    s->parse = OSVC_PARSE_STOPPED;
    s->next_clipboard_request = 1U;
    s->next_terminal_id = 1U;
    s->next_file_id = 1U;
    /* A STATUS before the first RESET has a token too. */
    fill_token(s, s->token);
}

void osvc_session_usb_reset(osvc_session_t *s,
                            uint8_t status[OSVC_USB_STATUS_LEN])
{
    uint8_t token[OSVC_TOKEN_LEN];
    fill_token(s, token);
    osvc_session_usb_reset_with_token(s, token, status);
}

void osvc_session_usb_reset_with_token(osvc_session_t *s,
                                       const uint8_t token[OSVC_TOKEN_LEN],
                                       uint8_t status[OSVC_USB_STATUS_LEN])
{
    end_session(s, "RESET");
    memcpy(s->token, token, OSVC_TOKEN_LEN);
    s->link = OSVC_LINK_RESETTING;
    s->counters.resets++;
    osvc_session_usb_status(s, status);
    /* The previous token's transfers must be cancelled and reaped before
     * the new session is admitted. */
    s->platform.reset_transport(s->platform.ctx);
}

void osvc_session_usb_status(const osvc_session_t *s,
                             uint8_t status[OSVC_USB_STATUS_LEN])
{
    memset(status, 0, OSVC_USB_STATUS_LEN);
    memcpy(status, "JKV5", 4U);
    osvc_phase_t phase;
    if (s->faulted) {
        phase = OSVC_PHASE_FAULTED;
    } else if (s->link == OSVC_LINK_ADMITTED) {
        phase = OSVC_PHASE_READY;
    } else if (s->link == OSVC_LINK_OPEN) {
        phase = OSVC_PHASE_ACTIVE;
    } else {
        phase = OSVC_PHASE_RESETTING;
    }
    status[4] = (uint8_t)phase;
    memcpy(status + 8, s->token, OSVC_TOKEN_LEN);
}

void osvc_session_transport_drained(osvc_session_t *s)
{
    if (s->link != OSVC_LINK_RESETTING || s->faulted) {
        return;
    }
    s->link = OSVC_LINK_ADMITTED;
    s->admitted_ms = now_ms(s);
    s->parse = OSVC_PARSE_SEARCH;
    s->searched = 0U;
    s->search_tail_fill = 0U;
    s->header_fill = 0U;
}

void osvc_session_end(osvc_session_t *s, const char *reason)
{
    end_session(s, reason);
}

void osvc_session_usb_reconfigured(osvc_session_t *s, bool by_kvm)
{
    /* The KVM's own USB restart does not count against the budget: the
     * next session switches back, and mounts would block video. */
    s->video_switch_back |= by_kvm;
    end_session(s, "the USB function was reconfigured");
    /* A new token tells the sender of the ended session that it is
     * superseded; no hello can use it before the next RESET. */
    fill_token(s, s->token);
}

void osvc_session_set_faulted(osvc_session_t *s, bool faulted)
{
    if (faulted) {
        end_session(s, "the USB function failed");
    }
    s->faulted = faulted;
}

void osvc_session_tick(osvc_session_t *s)
{
    const int64_t now = now_ms(s);
    if (s->link == OSVC_LINK_ADMITTED &&
        now - s->admitted_ms > OSVC_HANDSHAKE_TIMEOUT_MS) {
        end_session(s, "no session.hello within 15 seconds of admission");
        return;
    }
    if (s->link == OSVC_LINK_OPEN &&
        now - s->last_message_ms > OSVC_LIVENESS_TIMEOUT_MS) {
        end_session(s, "no message from the sender for 15 seconds");
        return;
    }
    if (s->parse == OSVC_PARSE_HELD) {
        /* Answers may wait unread behind the output: no request times out
         * before the session has read on. */
        (void)deliver_held(s);
        return;
    }
    for (size_t i = 0U; i < OSVC_CLIPBOARD_PENDING_MAX; ++i) {
        osvc_clipboard_request_t *request = &s->clipboard_requests[i];
        if (request->in_use && now >= request->deadline_ms) {
            request->in_use = false;
            if (s->platform.clipboard_done) {
                s->platform.clipboard_done(s->platform.ctx, request->request_id,
                                           OSVC_CLIPBOARD_FAILED, false, NULL,
                                           0U, false);
            }
        }
    }
    /* An answer that never came may still have started a shell, so the
     * timeout closes it on the sender too. */
    for (size_t i = 0U; i < OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        osvc_terminal_t *terminal = &s->terminals[i];
        if (terminal->state == OSVC_TERMINAL_OPENING &&
            now >= terminal->deadline_ms) {
            end_terminal(s, terminal, TERMINAL_NO_ANSWER, false, true);
        }
    }
    if (update_open(s, s->update.update_id) &&
        now - s->update.progress_ms >= OSVC_UPDATE_IDLE_TIMEOUT_MS) {
        end_update(s, OSVC_UPDATE_STATUS_FAILED,
                   "the update made no progress for 30 seconds", true);
    }
    osvc_file_drop_t *drop = &s->file_drop;
    if (drop->drop_id != 0U && !drop->attached && drop->ended < drop->count &&
        now >= drop->attach_deadline_ms) {
        end_transfers(s, OSVC_FILE_DONE_FAILED, true);
    }
    if (drop->drop_id != 0U &&
        now - drop->progress_ms >= OSVC_FILE_IDLE_TIMEOUT_MS) {
        if (drop->ended < drop->count) {
            end_transfers(s, OSVC_FILE_DONE_FAILED, true);
        } else if (!drop->attached) {
            /* Its channel never opened to hear the outcomes. */
            forget_drop(s);
        }
    }
    video_tick(s, now);
}

void osvc_session_apply_settings(osvc_session_t *s,
                                 const osvc_settings_t *settings)
{
    const osvc_settings_t previous = s->settings;
    s->settings = *settings;
    if (previous.video_enabled != settings->video_enabled) {
        /* Turning video off or on clears the retry budget and its block. */
        memset(&s->video_recovery, 0, sizeof(s->video_recovery));
        s->video_error[0] = '\0';
    }
    if (s->link != OSVC_LINK_OPEN) {
        return;
    }
    /* A service turned off ends here first. The sender ends its shells and
     * transfers when it gets the session.config below; transfers are also
     * cancelled one by one. */
    if (previous.terminal_enabled && !settings->terminal_enabled) {
        end_terminals(s, TERMINAL_DISABLED, false);
    }
    if (previous.files_enabled && !settings->files_enabled) {
        end_transfers(s, OSVC_FILE_DONE_FAILED, true);
        if (s->link != OSVC_LINK_OPEN) {
            return;
        }
    }
    if (previous.terminal_enabled != settings->terminal_enabled ||
        previous.files_enabled != settings->files_enabled ||
        strcmp(previous.capture_display, settings->capture_display) != 0) {
        if (!send_session_config(s)) {
            return;
        }
    }
    if (previous.clipboard_enabled != settings->clipboard_enabled &&
        s->clipboard_registered) {
        if (!send_clipboard_config(s)) {
            return;
        }
    }
    video_apply_settings(s, &previous);
    if (s->link != OSVC_LINK_OPEN) {
        return;
    }
    notify_state(s);
}

void osvc_session_set_fingerprint(osvc_session_t *s, const char *text)
{
    snprintf(s->fingerprint, sizeof(s->fingerprint), "%s", text ? text : "");
}

uint32_t osvc_session_clipboard_request(osvc_session_t *s, const uint8_t *text,
                                        size_t length,
                                        osvc_clipboard_status_t *status)
{
    *status = OSVC_CLIPBOARD_UNSUPPORTED;
    const uint32_t needed = text == NULL ? OSVC_CLIPBOARD_FLAG_READ :
                                           OSVC_CLIPBOARD_FLAG_WRITE;
    if (!osvc_session_clipboard_available(s) ||
        (s->clipboard.flags & needed) == 0U) {
        return 0U;
    }
    *status = OSVC_CLIPBOARD_FAILED;
    if (text != NULL && (length > OSVC_CLIPBOARD_MAX_TEXT ||
                         !osvc_utf8_valid(text, length))) {
        return 0U;
    }
    osvc_clipboard_request_t *slot = NULL;
    for (size_t i = 0U; i < OSVC_CLIPBOARD_PENDING_MAX; ++i) {
        if (!s->clipboard_requests[i].in_use) {
            slot = &s->clipboard_requests[i];
            break;
        }
    }
    if (slot == NULL) {
        return 0U;
    }
    uint32_t id = s->next_clipboard_request++;
    if (id == 0U) {
        id = s->next_clipboard_request++;
    }
    uint8_t request[4];
    osvc_put_u32(request, id);
    bool ok;
    if (text == NULL) {
        ok = send_message(s, OSVC_SERVICE_CLIPBOARD, OSVC_CLIPBOARD_GET,
                          request, sizeof(request));
    } else {
        uint8_t envelope[OSVC_ENVELOPE_LEN];
        osvc_put_envelope(envelope, OSVC_SERVICE_CLIPBOARD, OSVC_CLIPBOARD_SET,
                          (uint32_t)(sizeof(request) + length));
        ok = s->platform.write(s->platform.ctx, envelope, request,
                               sizeof(request), text, length);
        if (!ok) {
            end_session(s, "the IN endpoint did not drain");
        } else {
            s->counters.bytes_out +=
                OSVC_ENVELOPE_LEN + sizeof(request) + length;
        }
    }
    if (!ok) {
        return 0U;
    }
    slot->in_use = true;
    slot->request_id = id;
    slot->kind = text == NULL ? OSVC_CLIPBOARD_GET : OSVC_CLIPBOARD_SET;
    slot->deadline_ms = now_ms(s) + OSVC_CLIPBOARD_TIMEOUT_MS;
    *status = OSVC_CLIPBOARD_OK;
    return id;
}

bool osvc_session_open(const osvc_session_t *s)
{
    return s->link == OSVC_LINK_OPEN;
}

bool osvc_session_clipboard_available(const osvc_session_t *s)
{
    return s->link == OSVC_LINK_OPEN && s->clipboard_registered &&
           s->settings.clipboard_enabled;
}

bool osvc_session_terminal_available(const osvc_session_t *s)
{
    return s->link == OSVC_LINK_OPEN && s->terminal_shell &&
           s->settings.terminal_enabled && s->platform.terminal_output != NULL;
}

static bool terminal_size_valid(uint32_t rows, uint32_t cols)
{
    return rows != 0U && cols != 0U && rows <= UINT16_MAX &&
           cols <= UINT16_MAX;
}

static void put_terminal_size(uint8_t payload[OSVC_TERMINAL_SIZE_LEN],
                              uint32_t terminal_id, uint32_t rows,
                              uint32_t cols)
{
    osvc_put_u32(payload, terminal_id);
    osvc_put_u16(payload + 4, (uint16_t)rows);
    osvc_put_u16(payload + 6, (uint16_t)cols);
}

const char *osvc_session_terminal_open(osvc_session_t *s, uint32_t rows,
                                       uint32_t cols, uint32_t *terminal_id)
{
    *terminal_id = 0U;
    if (!osvc_session_terminal_available(s)) {
        return TERMINAL_UNAVAILABLE;
    }
    osvc_terminal_t *slot = NULL;
    for (size_t i = 0U; i < OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        if (s->terminals[i].state == OSVC_TERMINAL_FREE) {
            slot = &s->terminals[i];
            break;
        }
    }
    if (slot == NULL) {
        return TERMINAL_BUSY;
    }
    /* Never 0, and never again in this protocol session. */
    uint32_t id = s->next_terminal_id++;
    if (id == 0U) {
        id = s->next_terminal_id++;
    }
    /* The size is the browser's to report; the shell starts at its default
     * and is resized at once (Terminal service). */
    uint8_t payload[OSVC_TERMINAL_SIZE_LEN];
    put_terminal_size(payload, id, 0U, 0U);
    if (!send_message(s, OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_OPEN, payload,
                      sizeof(payload))) {
        return TERMINAL_NOT_SENT;
    }
    if (terminal_size_valid(rows, cols)) {
        put_terminal_size(payload, id, rows, cols);
        if (!send_message(s, OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_RESIZE,
                          payload, sizeof(payload))) {
            return TERMINAL_NOT_SENT;
        }
    }
    slot->terminal_id = id;
    slot->state = OSVC_TERMINAL_OPENING;
    slot->deadline_ms = now_ms(s) + OSVC_TERMINAL_OPEN_TIMEOUT_MS;
    *terminal_id = id;
    return NULL;
}

bool osvc_session_terminal_input(osvc_session_t *s, uint32_t terminal_id,
                                 const uint8_t *data, size_t length)
{
    osvc_terminal_t *terminal = find_terminal(s, terminal_id);
    if (terminal == NULL || s->link != OSVC_LINK_OPEN) {
        return false;
    }
    uint8_t head[4];
    osvc_put_u32(head, terminal_id);
    for (size_t offset = 0U; offset < length;) {
        const size_t n = length - offset < OSVC_TERMINAL_MAX_DATA ?
                             length - offset : OSVC_TERMINAL_MAX_DATA;
        if (!write_message(s, OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_INPUT, head,
                           sizeof(head), data + offset, n)) {
            end_terminal(s, terminal, TERMINAL_INPUT_LOST, false, false);
            end_session(s, IN_ENDPOINT_STUCK);
            return false;
        }
        offset += n;
    }
    return true;
}

bool osvc_session_terminal_resize(osvc_session_t *s, uint32_t terminal_id,
                                  uint32_t rows, uint32_t cols)
{
    if (find_terminal(s, terminal_id) == NULL || s->link != OSVC_LINK_OPEN ||
        !terminal_size_valid(rows, cols)) {
        return false;
    }
    uint8_t payload[OSVC_TERMINAL_SIZE_LEN];
    put_terminal_size(payload, terminal_id, rows, cols);
    return send_message(s, OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_RESIZE,
                        payload, sizeof(payload));
}

bool osvc_session_terminal_credit(osvc_session_t *s, uint32_t terminal_id,
                                  uint32_t bytes)
{
    if (find_terminal(s, terminal_id) == NULL || s->link != OSVC_LINK_OPEN) {
        return false;
    }
    uint8_t payload[OSVC_TERMINAL_CREDIT_LEN];
    osvc_put_u32(payload, terminal_id);
    osvc_put_u32(payload + 4, bytes);
    return send_message(s, OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_CREDIT,
                        payload, sizeof(payload));
}

void osvc_session_terminal_close(osvc_session_t *s, uint32_t terminal_id)
{
    osvc_terminal_t *terminal = find_terminal(s, terminal_id);
    if (terminal == NULL) {
        return;
    }
    memset(terminal, 0, sizeof(*terminal));
    if (s->link == OSVC_LINK_OPEN) {
        (void)send_terminal_close(s, terminal_id);
    }
}

bool osvc_session_file_available(const osvc_session_t *s)
{
    return s->link == OSVC_LINK_OPEN && s->file_receive &&
           s->settings.files_enabled && s->platform.file_credit != NULL &&
           s->platform.file_done != NULL;
}

/* A name for an error message, quoted: quotes, backslashes, control
 * characters and the bytes of invalid UTF-8 escaped, cut at a character. */
static void quote_name(char *out, size_t capacity, const uint8_t *name,
                       size_t length)
{
    const bool utf8 = osvc_utf8_valid(name, length);
    size_t o = 0U;
    out[o++] = '"';
    for (size_t i = 0U; i < length;) {
        const uint8_t c = name[i];
        size_t unit = 1U;
        if (utf8 && c >= 0xc0U) {
            unit = c >= 0xf0U ? 4U : c >= 0xe0U ? 3U : 2U;
        }
        /* Room for this character (at most 4 bytes), "...", the closing
         * quote and the terminator. */
        if (o + 4U + 3U + 2U > capacity) {
            memcpy(out + o, "...", 3U);
            o += 3U;
            break;
        }
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20U || c == 0x7fU || (c >= 0x80U && !utf8)) {
            static const char hex[] = "0123456789abcdef";
            out[o++] = '\\';
            out[o++] = 'x';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0fU];
        } else {
            memcpy(out + o, name + i, unit);
            o += unit;
        }
        i += unit;
    }
    out[o++] = '"';
    out[o] = '\0';
}

const char *osvc_session_file_drop(osvc_session_t *s,
                                   const osvc_file_entry_t *files, size_t count,
                                   uint32_t *drop_id, uint32_t *transfer_ids)
{
    *drop_id = 0U;
    if (!osvc_session_file_available(s)) {
        return FILE_UNAVAILABLE;
    }
    /* One drop at a time: the sender writes files one after another, so a
     * second drop would only queue, and the idle timeout could not tell
     * that wait from a stall. A drop whose outcomes wait for its channel
     * does not count. */
    if (s->file_drop.drop_id != 0U && s->file_drop.ended < s->file_drop.count) {
        return FILE_BUSY;
    }
    if (count == 0U) {
        return FILE_NO_FILES;
    }
    if (count > OSVC_FILE_DROP_MAX) {
        snprintf(s->file_error, sizeof(s->file_error),
                 "too many files for one drop: %zu (at most %u)", count,
                 OSVC_FILE_DROP_MAX);
        return s->file_error;
    }
    /* Each name counts at most a byte over the limit, so the sum of at most
     * OSVC_FILE_DROP_MAX of them cannot wrap. */
    size_t payload_length = OSVC_FILE_DROP_HEADER_LEN;
    for (size_t i = 0U; i < count; ++i) {
        const uint8_t *name = (const uint8_t *)files[i].name;
        if (!osvc_file_name_valid(name, files[i].name_length)) {
            char quoted[72];
            quote_name(quoted, sizeof(quoted), name, files[i].name_length);
            snprintf(s->file_error, sizeof(s->file_error),
                     "invalid file name %s", quoted);
            return s->file_error;
        }
        payload_length += OSVC_FILE_DROP_ENTRY_LEN +
                          (files[i].name_length > OSVC_MAX_CONTROL_PAYLOAD ?
                               OSVC_MAX_CONTROL_PAYLOAD + 1U :
                               files[i].name_length);
    }
    if (payload_length > OSVC_MAX_CONTROL_PAYLOAD) {
        snprintf(s->file_error, sizeof(s->file_error),
                 "too many file names for one drop: %zu bytes", payload_length);
        return s->file_error;
    }
    /* The drop's ID, then one per file, none 0 and none used before. */
    uint32_t first = s->next_file_id;
    if (first == 0U || first > UINT32_MAX - (uint32_t)count - 1U) {
        first = 1U;
    }
    s->next_file_id = first + (uint32_t)count + 1U;
    uint8_t *payload = cJSON_malloc(payload_length);
    if (payload == NULL) {
        return "send drop: out of memory";
    }
    osvc_put_u32(payload, (uint32_t)count);
    size_t o = OSVC_FILE_DROP_HEADER_LEN;
    for (size_t i = 0U; i < count; ++i) {
        osvc_put_u32(payload + o, first + 1U + (uint32_t)i);
        osvc_put_u64(payload + o + 4, files[i].size);
        osvc_put_u16(payload + o + 12, (uint16_t)files[i].name_length);
        memcpy(payload + o + OSVC_FILE_DROP_ENTRY_LEN, files[i].name,
               files[i].name_length);
        o += OSVC_FILE_DROP_ENTRY_LEN + files[i].name_length;
    }
    const bool sent = send_message(s, OSVC_SERVICE_FILE, OSVC_FILE_DROP,
                                   payload, payload_length);
    cJSON_free(payload);
    if (!sent) {
        return "send drop: " IN_ENDPOINT_STUCK;
    }
    /* A finished drop whose channel never opened is replaced. */
    forget_drop(s);
    osvc_file_drop_t *drop = &s->file_drop;
    drop->drop_id = first;
    drop->count = (uint32_t)count;
    drop->progress_ms = now_ms(s);
    drop->attach_deadline_ms = drop->progress_ms + OSVC_FILE_ATTACH_TIMEOUT_MS;
    for (size_t i = 0U; i < count; ++i) {
        drop->transfers[i].size = files[i].size;
        transfer_ids[i] = first + 1U + (uint32_t)i;
    }
    *drop_id = first;
    return NULL;
}

bool osvc_session_file_attach(osvc_session_t *s, uint32_t drop_id)
{
    osvc_file_drop_t *drop = &s->file_drop;
    if (drop_id == 0U || drop->drop_id != drop_id || drop->attached) {
        return false;
    }
    drop->attached = true;
    if (drop->ended < drop->count) {
        drop->progress_ms = now_ms(s);
    }
    for (size_t i = 0U; drop->drop_id == drop_id && i < drop->count; ++i) {
        const osvc_file_transfer_t *transfer = &drop->transfers[i];
        if (!transfer->ended) {
            report_credit(s, i);
        } else if (!transfer->reported) {
            report_done(s, i, NULL, 0U);
        }
    }
    return true;
}

bool osvc_session_file_data(osvc_session_t *s, uint32_t drop_id,
                            uint32_t transfer_id, const uint8_t *data,
                            size_t length)
{
    size_t index;
    if (drop_id == 0U || length == 0U || s->link != OSVC_LINK_OPEN ||
        !find_transfer(s, drop_id, transfer_id, &index)) {
        return false;
    }
    osvc_file_transfer_t *transfer = &s->file_drop.transfers[index];
    if (length > transfer->credit - transfer->sent ||
        length > transfer->size - transfer->sent) {
        /* A browser that sends beyond its credit loses the drop. */
        end_transfers(s, OSVC_FILE_DONE_FAILED, true);
        return false;
    }
    const uint64_t offset = transfer->sent;
    transfer->sent += length;
    s->file_drop.progress_ms = now_ms(s);
    uint8_t head[OSVC_CHUNK_HEADER_LEN];
    osvc_put_u32(head, transfer_id);
    for (size_t done = 0U; done < length;) {
        const size_t n = length - done < OSVC_CHUNK_MAX_DATA ?
                             length - done : OSVC_CHUNK_MAX_DATA;
        osvc_put_u64(head + 4, offset + done);
        if (!write_message(s, OSVC_SERVICE_FILE, OSVC_FILE_CHUNK, head,
                           sizeof(head), data + done, n)) {
            end_session(s, IN_ENDPOINT_STUCK);
            return false;
        }
        done += n;
    }
    return true;
}

bool osvc_session_file_cancel(osvc_session_t *s, uint32_t drop_id,
                              uint32_t transfer_id)
{
    size_t index;
    if (drop_id == 0U || !find_transfer(s, drop_id, transfer_id, &index)) {
        return false;
    }
    end_transfer(s, index, OSVC_FILE_DONE_CANCELLED, NULL, 0U, true);
    return true;
}

void osvc_session_file_channel_closed(osvc_session_t *s, uint32_t drop_id)
{
    if (drop_id == 0U || s->file_drop.drop_id != drop_id) {
        return;
    }
    /* Nobody is left to tell. */
    s->file_drop.attached = false;
    end_transfers(s, OSVC_FILE_DONE_CANCELLED, true);
    if (s->file_drop.drop_id == drop_id) {
        forget_drop(s);
    }
}

uint32_t osvc_session_file_end_unattached(osvc_session_t *s, uint32_t drop_id,
                                          osvc_file_status_t status)
{
    if (drop_id == 0U || s->file_drop.drop_id != drop_id ||
        s->file_drop.attached) {
        return 0U;
    }
    const uint32_t count = s->file_drop.count;
    end_transfers(s, status, true);
    if (s->file_drop.drop_id == drop_id) {
        forget_drop(s);
    }
    return count;
}

bool osvc_session_file_size(const osvc_session_t *s, uint32_t drop_id,
                            uint32_t transfer_id, uint64_t *size)
{
    const osvc_file_drop_t *drop = &s->file_drop;
    if (drop_id == 0U || drop->drop_id != drop_id ||
        transfer_id <= drop_id || transfer_id - drop_id > drop->count) {
        return false;
    }
    *size = drop->transfers[transfer_id - drop_id - 1U].size;
    return true;
}

void osvc_session_file_fail(osvc_session_t *s, uint32_t drop_id)
{
    if (drop_id == 0U || s->file_drop.drop_id != drop_id) {
        return;
    }
    end_transfers(s, OSVC_FILE_DONE_FAILED, true);
}

bool osvc_session_update_available(const osvc_session_t *s)
{
    return s->link == OSVC_LINK_OPEN && s->update_install;
}

const char *osvc_session_update_begin(osvc_session_t *s, uint64_t size,
                                      const uint8_t sha256[32],
                                      const char *version,
                                      uint32_t *update_id)
{
    *update_id = 0U;
    const size_t version_length = strlen(version);
    if (!osvc_session_update_available(s)) {
        return UPDATE_UNAVAILABLE;
    }
    if (update_open(s, s->update.update_id)) {
        return UPDATE_BUSY;
    }
    if (size == 0U || version_length > OSVC_UPDATE_VERSION_MAX) {
        return "invalid OS Services release";
    }
    if (++s->next_update_id == 0U) {
        s->next_update_id = 1U;
    }
    s->update = (osvc_update_t){
        .update_id = s->next_update_id,
        .size = size,
        .progress_ms = now_ms(s),
    };
    uint8_t head[OSVC_UPDATE_BEGIN_FIXED_LEN];
    osvc_put_u32(head, s->update.update_id);
    osvc_put_u64(head + 4, size);
    memcpy(head + 12, sha256, 32U);
    if (!write_message(s, OSVC_SERVICE_UPDATE, OSVC_UPDATE_BEGIN, head,
                       sizeof(head), (const uint8_t *)version,
                       version_length)) {
        end_session(s, IN_ENDPOINT_STUCK);
        return IN_ENDPOINT_STUCK;
    }
    *update_id = s->update.update_id;
    return NULL;
}

size_t osvc_session_update_send(osvc_session_t *s, uint32_t update_id,
                                const uint8_t *data, size_t length)
{
    osvc_update_t *update = &s->update;
    size_t done = 0U;
    while (done < length && update_open(s, update_id) && update->ready) {
        const uint64_t limit =
            update->credit < update->size ? update->credit : update->size;
        size_t n = length - done < OSVC_CHUNK_MAX_DATA ? length - done :
                                                         OSVC_CHUNK_MAX_DATA;
        if (limit - update->sent < n) {
            n = (size_t)(limit - update->sent);
        }
        if (n == 0U) {
            break;
        }
        uint8_t head[OSVC_CHUNK_HEADER_LEN];
        osvc_put_u32(head, update_id);
        osvc_put_u64(head + 4, update->sent);
        if (!write_message(s, OSVC_SERVICE_UPDATE, OSVC_UPDATE_CHUNK, head,
                           sizeof(head), data + done, n)) {
            end_session(s, IN_ENDPOINT_STUCK);
            break;
        }
        update->sent += n;
        update->progress_ms = now_ms(s);
        done += n;
    }
    return done;
}

void osvc_session_update_cancel(osvc_session_t *s, uint32_t update_id,
                                const char *reason)
{
    if (update_open(s, update_id)) {
        end_update(s, OSVC_UPDATE_STATUS_CANCELLED, reason, true);
    }
}

const char *osvc_clipboard_status_name(osvc_clipboard_status_t status)
{
    switch (status) {
    case OSVC_CLIPBOARD_OK:
        return "ok";
    case OSVC_CLIPBOARD_UNSUPPORTED:
        return "unsupported";
    case OSVC_CLIPBOARD_EMPTY:
        return "empty";
    case OSVC_CLIPBOARD_DENIED:
        return "denied";
    default:
        return "failed";
    }
}

const char *osvc_file_status_name(osvc_file_status_t status)
{
    switch (status) {
    case OSVC_FILE_DONE_OK:
        return "ok";
    case OSVC_FILE_DONE_CANCELLED:
        return "cancelled";
    default:
        return "failed";
    }
}

const char *osvc_target_os_name(osvc_target_os_t os)
{
    switch (os) {
    case OSVC_OS_WINDOWS:
        return "windows";
    case OSVC_OS_MACOS:
        return "macos";
    case OSVC_OS_LINUX:
        return "linux";
    default:
        return "unknown";
    }
}
