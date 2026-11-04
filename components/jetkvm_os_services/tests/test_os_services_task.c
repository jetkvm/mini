/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* The service task's settings and video state (jetkvm_os_services.c), with
 * ESP-IDF faked by fake_idf.h and the task's loop run on this thread. */

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

#include "../jetkvm_os_services.c"
#include "os_services_release.h"
#include "rpc_events.h"

/* JETKVM_RPC_EVENT_OUTPUT_MAX in webrtc_transport/webrtc_data_channels.c. */
#define EVENT_OUTPUT_MAX 1280U

static TaskFunction_t s_task;
static jmp_buf s_task_stop;
static unsigned s_task_passes;

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry,
                                           const char *name, uint32_t stack,
                                           void *arg, UBaseType_t priority,
                                           TaskHandle_t *task, BaseType_t core,
                                           UBaseType_t caps)
{
    s_task = entry;
    *task = (TaskHandle_t)&s_task;
    return pdPASS;
}

void xTaskNotifyGive(TaskHandle_t task) {}

/* What each wait returns: 0 when it ends at its timeout. */
static uint32_t s_notify_result = 1U;

/* Where the task waits: it returns after its last pass. */
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    if (s_task_passes == 0U) {
        longjmp(s_task_stop, 1);
    }
    s_task_passes--;
    return s_notify_result;
}

static void run_task(unsigned passes)
{
    s_task_passes = passes;
    if (setjmp(s_task_stop) == 0) {
        s_task(NULL);
    }
}

esp_err_t osvc_usb_init(TaskHandle_t task) { return ESP_OK; }
void osvc_usb_get_events(osvc_usb_events_t *events)
{
    memset(events, 0, sizeof(*events));
}
void osvc_usb_publish_status(uint32_t reset_sequence,
                             const uint8_t status[OSVC_USB_STATUS_LEN]) {}
void osvc_usb_restart(void) {}
void osvc_usb_stop(void) {}
bool osvc_usb_receive(const uint8_t **data, size_t *length) { return false; }
void osvc_usb_receive_done(void) {}
bool osvc_usb_write(const uint8_t *const parts[], const size_t lengths[],
                    size_t count, uint32_t timeout_ms)
{
    return true;
}
int64_t osvc_usb_in_waiting_ms(void) { return 0; }
void osvc_usb_get_counters(osvc_usb_counters_t *counters)
{
    memset(counters, 0, sizeof(*counters));
}
esp_err_t osvc_identity_load(void) { return ESP_OK; }
bool osvc_identity_public_key(uint8_t key[OSVC_PUBLIC_KEY_LEN])
{
    return false;
}
bool osvc_identity_sign(const uint8_t *message, size_t length,
                        uint8_t signature[OSVC_SIGNATURE_LEN])
{
    return false;
}
void osvc_identity_fingerprint(char out[OSVC_FINGERPRINT_TEXT_CAPACITY])
{
    snprintf(out, OSVC_FINGERPRINT_TEXT_CAPACITY, "AA:BB:CC:DD:EE:FF:00:11");
}
/* The capture task may clear the WebRTC mode as soon as a guest display is
 * queued, before this task's next view refresh. */
esp_err_t jetkvm_video_present_guest(const uint8_t edid[256])
{
    webrtc_transport_clear_mode();
    return ESP_OK;
}
/* What the service video hooks asked of the capture task and the
 * transport, in order. */
static char s_calls[256];
static void called(const char *call)
{
    strcat(s_calls, call);
    strcat(s_calls, " ");
}
esp_err_t jetkvm_video_restore_physical(void)
{
    called("restore");
    return ESP_OK;
}
bool jetkvm_video_guest_display_presented(void) { return false; }
bool jetkvm_video_guest_owns_display(void) { return false; }
static esp_err_t s_lend_error;
static uint8_t s_capture[CONFIG_WEBRTC_TRANSPORT_AU_SLOTS][16];
esp_err_t jetkvm_video_lend_guest_buffers(uint8_t *buffers[], size_t count,
                                          size_t *bytes)
{
    assert(count == CONFIG_WEBRTC_TRANSPORT_AU_SLOTS);
    called("lend");
    for (size_t i = 0U; i < count; ++i) {
        buffers[i] = s_capture[i];
    }
    *bytes = 3110400U;
    return s_lend_error;
}
void jetkvm_video_return_guest_buffers(void) { called("return"); }
static esp_err_t s_buffers_error;
esp_err_t webrtc_transport_set_guest_buffers(uint8_t *const *buffers,
                                             size_t bytes, uint32_t timeout_ms)
{
    assert(buffers == NULL ||
           (buffers[0] == s_capture[0] && bytes == 3110400U));
    called(buffers != NULL ? "lent-slots" : "own-slots");
    return s_buffers_error;
}
bool jetkvm_video_capture_suspended(void) { return false; }
esp_err_t webrtc_transport_set_guest_mode(uint16_t width, uint16_t height,
                                          uint16_t fps)
{
    called("mode");
    return ESP_OK;
}
/* The transport's switching flag, and what it was at each mode clear. */
static bool s_switching;
static unsigned s_mode_clears;
static unsigned s_clears_while_switching;

void webrtc_transport_clear_mode(void)
{
    s_mode_clears++;
    s_clears_while_switching += s_switching ? 1U : 0U;
}
/* A slot of this size, or none without a viewer (0). */
static size_t s_slot_bytes;
static uint8_t s_slot[16];
static unsigned s_slot_cancels;
esp_err_t webrtc_transport_reserve_guest_access_unit(
    uint16_t width, uint16_t height, uint16_t fps,
    webrtc_transport_au_reservation_t *reservation)
{
    *reservation = (webrtc_transport_au_reservation_t){
        .data = s_slot_bytes != 0U ? s_slot : NULL,
        .capacity = s_slot_bytes,
    };
    return s_slot_bytes != 0U ? ESP_OK : ESP_ERR_INVALID_STATE;
}
bool webrtc_transport_commit_cpu_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t size,
    const app_h264_frame_meta_t *meta)
{
    return false;
}
void webrtc_transport_cancel_cpu_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t written)
{
    s_slot_cancels++;
    *reservation = (webrtc_transport_au_reservation_t){0};
}
void webrtc_transport_note_video_gap(void) {}
uint32_t jetkvm_video_quality_get_permille(void) { return 0U; }
bool webrtc_transport_guest_bitrate(uint32_t ceiling_bps, bool adaptive,
                                    uint32_t *target_bps, uint32_t *sent_bps,
                                    bool *cut)
{
    (void)adaptive;
    *target_bps = ceiling_bps;
    *sent_bps = 0U;
    *cut = false;
    return false;
}
bool webrtc_transport_take_sync_idr_request(uint32_t *generation)
{
    return false;
}
void webrtc_transport_complete_sync_idr_production(uint32_t generation,
                                                   bool delivered)
{
}
void webrtc_transport_set_video_switching(bool switching)
{
    s_switching = switching;
}
void webrtc_transport_set_guest_video_source(bool (*source)(cJSON *object))
{
}

/* A change the browser makes while the task is stalled in a USB write,
 * behind clipboard requests that fill the command queue, still reaches the
 * session. */
static void test_settings_apply_with_full_queue(void)
{
    for (size_t i = 0U; i < COMMAND_QUEUE_LENGTH; ++i) {
        send_command(COMMAND_CLIPBOARD);
    }
    assert(s_osvc.session->settings.clipboard_enabled);
    cJSON *request = cJSON_Parse("{\"clipboard_enabled\":false}");
    cJSON *applied = cJSON_CreateObject();
    assert(jetkvm_os_services_set_settings_json(request, applied, NULL) ==
           NULL);
    cJSON_Delete(request);
    cJSON_Delete(applied);
    run_task(1U);
    assert(!s_osvc.session->settings.clipboard_enabled);
}

/* The update relay (os_services_relay.c, device only) with the longest
 * guestUpdate: 64-byte versions and error, the largest sizes. */
esp_err_t osvc_relay_init(TaskHandle_t service_task)
{
    return ESP_OK;
}
bool osvc_relay_service(osvc_session_t *session)
{
    return false;
}
uint32_t osvc_relay_generation(void)
{
    return 0U;
}
const char *osvc_relay_start(void)
{
    return NULL;
}
bool osvc_relay_add_state(cJSON *object)
{
    static const osvc_update_sender_t sender = {"0.4.1",
                                                "linux-x86_64-appimage"};
    osvc_release_t newest = {0};
    osvc_update_progress_t progress = {
        .phase = OSVC_UPDATE_PHASE_DOWNLOADING,
        .sent = OSVC_RELEASE_PACKAGE_MAX_BYTES,
        .size = OSVC_RELEASE_PACKAGE_MAX_BYTES,
    };
    memset(newest.version, '9', OSVC_UPDATE_VERSION_MAX);
    memcpy(progress.version, newest.version, sizeof(progress.version));
    memset(progress.error, 'e', sizeof(progress.error) - 1U);
    return osvc_update_render(object, &newest, &sender, &progress, true);
}

static void set_settings(const char *json)
{
    cJSON *request = cJSON_Parse(json);
    cJSON *applied = cJSON_CreateObject();
    assert(jetkvm_os_services_set_settings_json(request, applied, NULL) ==
           NULL);
    cJSON_Delete(request);
    cJSON_Delete(applied);
}

/* Video turned off and on while the task is stalled clears the retry
 * budget and its error, as two separate passes do. */
static void test_video_off_on_between_passes(void)
{
    osvc_session_t *session = s_osvc.session;
    assert(session->settings.video_enabled);
    memset(&session->video_recovery, 0xff, sizeof(session->video_recovery));
    strcpy(session->video_error, "blocked");
    set_settings("{\"video_enabled\":false}");
    set_settings("{\"video_enabled\":true}");
    run_task(1U);
    assert(session->settings.video_enabled);
    const osvc_video_recovery_t cleared = {0};
    assert(memcmp(&session->video_recovery, &cleared, sizeof(cleared)) == 0);
    assert(session->video_error[0] == '\0');
}

/* Service video stops for a switch to hardware capture: the transport hears
 * of the switch before its mode is cleared, so no videoInputState in between
 * says no_signal, and hears of its end, also when the view never showed it. */
static void test_video_stop_is_a_switch(void)
{
    /* A switch that started in this pass. */
    s_osvc.session->video_source = OSVC_SOURCE_SWITCHING_TO_GUEST;
    osvc_session_end(s_osvc.session, "test");
    assert(s_mode_clears == 1U && s_clears_while_switching == 1U);
    run_task(1U);
    assert(s_osvc.view->source == OSVC_SOURCE_PHYSICAL && !s_switching);
}

/* Service video starts: the transport hears of the switch before the
 * capture task can clear its mode for the guest display. */
static void test_video_start_is_a_switch(void)
{
    const osvc_platform_t *platform = &s_osvc.session->platform;
    s_mode_clears = s_clears_while_switching = 0U;
    assert(platform->display_present(platform->ctx, 1920U, 1080U, 1U));
    assert(s_mode_clears == 1U && s_clears_while_switching == 1U);
}

/* Service video's access units go into capture buffers the guest display
 * leaves idle: the first stream borrows them, later ones reuse them, and
 * they go back, once the transport has left them, before hardware capture
 * returns. A transport that cannot leave them in time keeps the loan, and
 * the restore tries again. */
static void test_video_borrows_capture_buffers(void)
{
    const osvc_platform_t *platform = &s_osvc.session->platform;
    s_calls[0] = '\0';
    s_lend_error = ESP_ERR_INVALID_STATE;
    assert(platform->video_stream(platform->ctx, 3840U, 2160U, 30U) == 0U);
    s_lend_error = ESP_OK;
    s_buffers_error = ESP_ERR_TIMEOUT;
    assert(platform->video_stream(platform->ctx, 3840U, 2160U, 30U) == 0U);
    assert(strcmp(s_calls, "lend lend lent-slots return ") == 0);

    s_calls[0] = '\0';
    s_buffers_error = ESP_OK;
    assert(platform->video_stream(platform->ctx, 3840U, 2160U, 30U) ==
           3110400U);
    assert(platform->video_stream(platform->ctx, 1920U, 1080U, 30U) ==
           3110400U);
    platform->video_stopped(platform->ctx);
    platform->display_restore(platform->ctx);
    assert(strcmp(s_calls, "lend lent-slots mode mode own-slots return "
                           "restore ") == 0);

    s_calls[0] = '\0';
    assert(platform->video_stream(platform->ctx, 3840U, 2160U, 30U) ==
           3110400U);
    s_buffers_error = ESP_ERR_TIMEOUT;
    platform->video_stopped(platform->ctx);
    s_buffers_error = ESP_OK;
    platform->display_restore(platform->ctx);
    platform->display_restore(platform->ctx);
    assert(strcmp(s_calls, "lend lent-slots mode own-slots own-slots return "
                           "restore restore ") == 0);

    /* Neither the stop nor the restore got the slots back in time: the
     * task tries again once the guest display is gone, so hardware capture
     * does not stay down. */
    s_calls[0] = '\0';
    assert(platform->video_stream(platform->ctx, 3840U, 2160U, 30U) ==
           3110400U);
    s_buffers_error = ESP_ERR_TIMEOUT;
    platform->video_stopped(platform->ctx);
    platform->display_restore(platform->ctx);
    s_buffers_error = ESP_OK;
    run_task(1U);
    run_task(1U);
    assert(strcmp(s_calls, "lend lent-slots mode own-slots own-slots restore "
                           "own-slots return ") == 0);
}

/* A slot smaller than the frame is never written past: the frame is
 * skipped as if no viewer took it. */
static void test_video_frame_never_overruns_its_slot(void)
{
    const osvc_platform_t *platform = &s_osvc.session->platform;
    bool busy = true;
    s_slot_bytes = 4096U;
    assert(platform->frame_reserve(platform->ctx, 4096U, &busy) == s_slot &&
           !busy);
    platform->frame_cancel(platform->ctx, 0U);
    assert(platform->frame_reserve(platform->ctx, 4097U, &busy) == NULL &&
           !busy && s_slot_cancels == 2U);
    s_slot_bytes = 0U;
}

static double guest_video_stat(const char *name)
{
    cJSON *object = cJSON_CreateObject();
    assert(add_guest_video_stats(object));
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    assert(cJSON_IsNumber(item));
    const double value = item->valuedouble;
    cJSON_Delete(object);
    return value;
}

/* A frame that waits for a slot wakes the task at the slot's release or at
 * the next tick; /webrtc/stats counts the waits that ended at the tick.
 * Another wait that times out is not one of them. */
static void test_slot_wait_tick_wakeups(void)
{
    osvc_session_t *session = s_osvc.session;
    const osvc_parse_state_t parse = session->parse;
    session->parse = OSVC_PARSE_FRAME_WAIT;
    s_notify_result = 0U;
    run_task(1U);
    run_task(1U);
    s_notify_result = 1U;
    run_task(1U);
    assert(session->video_counters.slot_wait_tick_wakeups == 2U);
    session->parse = parse;
    s_notify_result = 0U;
    run_task(1U);
    s_notify_result = 1U;
    assert(session->video_counters.slot_wait_tick_wakeups == 2U);
    assert(guest_video_stat("slotWaitTickWakeups") == 2.0);
    assert(guest_video_stat("senderSkippedFrames") == 0.0);
}

static size_t longest_video_input_event(void)
{
    webrtc_rpc_event_snapshot_t snapshot = {
        .video = {.ready = true, .streaming = WEBRTC_RPC_VIDEO_STOPPING,
                  .width = UINT16_MAX, .height = UINT16_MAX,
                  .fps = UINT16_MAX},
    };
    char event[EVENT_OUTPUT_MAX];
    size_t ready = 0U;
    size_t idle = 0U;
    assert(webrtc_rpc_event_render(WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
                                   event, sizeof(event), &ready));
    snapshot.video.ready = false;
    assert(webrtc_rpc_event_render(WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
                                   event, sizeof(event), &idle));
    return ready > idle ? ready : idle;
}

/* The longest sender info session.info admits, escaped or not, leaves the
 * OS Services fields room in the videoInputState event, sender, source and
 * video error included. */
static void test_sender_fits_the_event(void)
{
    /* The fields go in as ",members" with the event's NUL after them. */
    const size_t room = EVENT_OUTPUT_MAX - 1U - longest_video_input_event();
    view_t *view = s_osvc.view;
    view->connected = view->clipboard = view->sender = true;
    view->terminal = view->file_receive = true;
    view->source = OSVC_SOURCE_SWITCHING_TO_PHYSICAL;
    /* The device writes these errors itself: nothing to escape. */
    memset(view->video_error, 'e', sizeof(view->video_error) - 1U);
    view->video_error[sizeof(view->video_error) - 1U] = '\0';
    view->display_count = OSVC_INFO_DISPLAYS_MAX;
    for (const char *fill = "\x01\"a"; *fill != '\0'; ++fill) {
        for (size_t length = 1U; length <= OSVC_INFO_DISPLAY_MAX;
             length *= 4U) {
            memset(view->version, *fill, OSVC_INFO_VERSION_MAX);
            for (size_t i = 0U; i < OSVC_INFO_DISPLAYS_MAX; ++i) {
                char *name = view->displays + i * (length + 1U);
                memset(name, *fill, length);
                name[length] = '\0';
            }
            cJSON *fields = cJSON_CreateObject();
            assert(jetkvm_os_services_add_video_state(fields, NULL));
            char *members = cJSON_PrintUnformatted(fields);
            assert(strlen(members) - 1U <= room);
            assert(strstr(members, "\"version\":") != NULL);
            cJSON_free(members);
            cJSON_Delete(fields);
        }
    }
}

int main(void)
{
    assert(jetkvm_os_services_init() == ESP_OK);
    test_settings_apply_with_full_queue();
    test_video_off_on_between_passes();
    test_video_stop_is_a_switch();
    test_video_start_is_a_switch();
    test_video_borrows_capture_buffers();
    test_video_frame_never_overruns_its_slot();
    test_slot_wait_tick_wakeups();
    test_sender_fits_the_event();
    printf("os_services task tests passed\n");
    return 0;
}
