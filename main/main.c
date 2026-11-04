/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/startup_internal.h"
#include "nvs_flash.h"
#include "soc/lp_clkrst_reg.h"
#include "soc/soc.h"
#include "jetkvm_console.h"
#include "jetkvm_factory_reset.h"
#include "jetkvm_flash_read_mode.h"
#include "jetkvm_access.h"
#include "jetkvm_network_settings.h"

#include "jetkvm_control.h"
#include "jetkvm_board_config.h"
#include "jetkvm_video.h"
#include "jetkvm_video_quality.h"
#include "jetkvm_storage_download.h"
#include "jetkvm_virtual_media.h"
#include "jetkvm_wol.h"
#include "device_api.h"
#include "device_contract.h"
#include "jetkvm_board_network.h"
#include "jetkvm_cloud_service.h"
#include "jetkvm_device_state.h"
#include "jetkvm_hid_input.h"
#include "jetkvm_hid_profiles.h"
#include "jetkvm_hdmi_bridge.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_mqtt.h"
#include "jetkvm_tailscale.h"
#include "jetkvm_oidc.h"
#include "jetkvm_ota.h"
#include "jetkvm_settings.h"
#include "jetkvm_time_sync.h"
#include "jetkvm_usb_debugging.h"
#include "jetkvm_usb_hid.h"
#include "jetkvm_usb_service.h"
#include "jetkvm_audio.h"
#include "jetkvm_os_services.h"
#include "webrtc_transport.h"

#if JETKVM_BOARD_WIFI
#include "jetkvm_c5_wifi.h"
#include "jetkvm_screen.h"
#endif


static const char *TAG = "hdmi_csi";
static jetkvm_oidc_validator_t *s_cloud_oidc_validator;
static jetkvm_tailscale_context_t s_tailscale_context;

/* The names a browser may use for the device besides its addresses: the
 * hostname it gives the LAN and, on a tailnet, its Tailscale names. */
static size_t access_host_names(char (*names)[JETKVM_ACCESS_HOST_NAME_CAPACITY], size_t capacity)
{
    size_t count = 0;
    jetkvm_network_settings_t network;
    if (count < capacity && jetkvm_network_settings_get(&network, NULL) && network.hostname[0]) {
        snprintf(names[count++], JETKVM_ACCESS_HOST_NAME_CAPACITY, "%s", network.hostname);
    }
    jetkvm_tailscale_status_t *tailnet = s_tailscale_context.get_status ?
        malloc(sizeof(*tailnet)) : NULL;
    if (tailnet && s_tailscale_context.get_status(tailnet, s_tailscale_context.ctx) &&
        tailnet->ipv4[0]) {
        const char *own[] = {tailnet->hostname, tailnet->dns_name};
        for (size_t i = 0; i < 2 && count < capacity; ++i) {
            if (own[i][0]) snprintf(names[count++], JETKVM_ACCESS_HOST_NAME_CAPACITY, "%s", own[i]);
        }
        /* MagicDNS also answers the first label of the machine name, which
         * differs from the host name after a rename or a name collision. */
        size_t machine = strcspn(tailnet->dns_name, ".");
        if (machine && count < capacity) {
            snprintf(names[count++], JETKVM_ACCESS_HOST_NAME_CAPACITY, "%.*s", (int)machine, tailnet->dns_name);
        }
    }
    free(tailnet);
    return count;
}

_Static_assert(JETKVM_EXTRA_HOSTNAME_CAPACITY == JETKVM_ACCESS_HOST_NAME_CAPACITY &&
               JETKVM_EXTRA_HOSTNAMES_MAX == JETKVM_ACCESS_EXTRA_HOST_NAMES,
               "the RPC carries the access gate's names");
static size_t extra_hostnames_get(char (*names)[JETKVM_EXTRA_HOSTNAME_CAPACITY], size_t capacity, void *ctx)
{
    (void)ctx;
    return jetkvm_access_get_extra_host_names(names, capacity);
}
static const char *extra_hostnames_set(const char *const *names, size_t count, void *ctx)
{
    (void)ctx;
    return jetkvm_access_set_extra_host_names(names, count);
}

#define P4_STAGED_DIAGNOSTIC 0
#define P4_SKIP_VIDEO_DIAGNOSTIC 0
#define P4_SKIP_TF_DIAGNOSTIC 0
#define P4_SKIP_TARGET_USB_DIAGNOSTIC 0

#if P4_STAGED_DIAGNOSTIC
#define P4_DIAG_IDLE_SECONDS 120U
#define P4_DIAG_CPU_SECONDS 180U
#define P4_DIAG_PSRAM_SECONDS 180U
#define P4_DIAG_PSRAM_BYTES (16U * 1024U * 1024U)

typedef enum {
    P4_DIAG_WORK_CPU,
    P4_DIAG_WORK_PSRAM,
} p4_diag_work_t;

typedef struct {
    p4_diag_work_t work;
    uint32_t core;
    volatile uint32_t *words;
    size_t word_count;
} p4_diag_worker_t;

static volatile bool s_p4_diag_run;
static volatile uint32_t s_p4_diag_passes[2];
static volatile uint32_t s_p4_diag_errors[2];
static p4_diag_worker_t s_p4_diag_workers[2];

static uint32_t p4_diag_xorshift(uint32_t value)
{
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

static void p4_diag_worker_task(void *arg)
{
    p4_diag_worker_t *worker = arg;
    uint32_t state = 0x9e3779b9U ^ (worker->core * 0x85ebca6bU);

    while (__atomic_load_n(&s_p4_diag_run, __ATOMIC_ACQUIRE)) {
        if (worker->work == P4_DIAG_WORK_CPU) {
            for (uint32_t i = 0; i < 250000U; ++i) {
                state = p4_diag_xorshift(state) + 0x7f4a7c15U;
            }
            if (state == 0U) {
                __atomic_fetch_add(&s_p4_diag_errors[worker->core], 1U,
                                   __ATOMIC_RELAXED);
            }
        } else {
            uint32_t pattern =
                0xa5a50000U ^ (worker->core << 12) ^
                __atomic_load_n(&s_p4_diag_passes[worker->core],
                                __ATOMIC_RELAXED);
            for (size_t i = 0; i < worker->word_count; ++i) {
                worker->words[i] = pattern ^ (uint32_t)i * 0x9e3779b9U;
            }
            for (size_t i = 0; i < worker->word_count; ++i) {
                uint32_t expected = pattern ^ (uint32_t)i * 0x9e3779b9U;
                if (worker->words[i] != expected) {
                    __atomic_fetch_add(&s_p4_diag_errors[worker->core], 1U,
                                       __ATOMIC_RELAXED);
                    break;
                }
            }
        }
        __atomic_fetch_add(&s_p4_diag_passes[worker->core], 1U,
                           __ATOMIC_RELAXED);
        vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

static void p4_diag_observe(const char *stage, uint32_t seconds)
{
    int64_t start_us = esp_timer_get_time();
    for (uint32_t elapsed = 0; elapsed < seconds; elapsed += 5U) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGW(TAG,
                 "P4_DIAG stage=%s elapsed=%" PRIu32
                 "s uptime=%" PRId64 "s passes=%" PRIu32 "/%" PRIu32
                 " errors=%" PRIu32 "/%" PRIu32,
                 stage, elapsed + 5U, esp_timer_get_time() / 1000000,
                 __atomic_load_n(&s_p4_diag_passes[0], __ATOMIC_RELAXED),
                 __atomic_load_n(&s_p4_diag_passes[1], __ATOMIC_RELAXED),
                 __atomic_load_n(&s_p4_diag_errors[0], __ATOMIC_RELAXED),
                 __atomic_load_n(&s_p4_diag_errors[1], __ATOMIC_RELAXED));
    }
    ESP_LOGW(TAG, "P4_DIAG stage=%s complete duration=%" PRId64 "ms",
             stage, (esp_timer_get_time() - start_us) / 1000);
}

static void p4_diag_start_workers(p4_diag_work_t work,
                                  volatile uint32_t *memory,
                                  size_t total_words)
{
    memset((void *)s_p4_diag_passes, 0, sizeof(s_p4_diag_passes));
    memset((void *)s_p4_diag_errors, 0, sizeof(s_p4_diag_errors));
    __atomic_store_n(&s_p4_diag_run, true, __ATOMIC_RELEASE);
    for (uint32_t core = 0; core < 2U; ++core) {
        s_p4_diag_workers[core] = (p4_diag_worker_t) {
            .work = work,
            .core = core,
            .words = memory ? memory + (total_words / 2U) * core : NULL,
            .word_count = memory ? total_words / 2U : 0U,
        };
        BaseType_t created = xTaskCreatePinnedToCore(
            p4_diag_worker_task, core == 0U ? "diag0" : "diag1", 4096,
            &s_p4_diag_workers[core], tskIDLE_PRIORITY + 2, NULL, core);
        ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
}

static void p4_diag_stop_workers(void)
{
    __atomic_store_n(&s_p4_diag_run, false, __ATOMIC_RELEASE);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void p4_staged_diagnostic(void)
{
    ESP_LOGW(TAG,
             "P4_DIAG begin: no TF/network/USB/HDMI/screen/PPA; "
             "idle=%us cpu=%us psram=%us",
             P4_DIAG_IDLE_SECONDS, P4_DIAG_CPU_SECONDS,
             P4_DIAG_PSRAM_SECONDS);
    p4_diag_observe("idle", P4_DIAG_IDLE_SECONDS);

    p4_diag_start_workers(P4_DIAG_WORK_CPU, NULL, 0U);
    p4_diag_observe("dual_cpu", P4_DIAG_CPU_SECONDS);
    p4_diag_stop_workers();

    volatile uint32_t *memory = heap_caps_malloc(
        P4_DIAG_PSRAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(memory != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGW(TAG, "P4_DIAG allocated PSRAM=%u bytes at %p",
             P4_DIAG_PSRAM_BYTES, (const void *)memory);
    p4_diag_start_workers(P4_DIAG_WORK_PSRAM, memory,
                          P4_DIAG_PSRAM_BYTES / sizeof(uint32_t));
    p4_diag_observe("dual_psram", P4_DIAG_PSRAM_SECONDS);
    p4_diag_stop_workers();
    heap_caps_free((void *)memory);

    ESP_LOGW(TAG, "P4_DIAG all stages passed; holding idle");
    for (;;) {
        p4_diag_observe("complete_idle", 60U);
    }
}
#endif


static bool settings_rpc_get_keyboard_layout(char *output,
                                             size_t output_capacity,
                                             void *ctx)
{
    (void)ctx;
    return jetkvm_settings_get_keyboard_layout(output, output_capacity) ==
           ESP_OK;
}

static jetkvm_keyboard_layout_change_result_t
settings_rpc_set_keyboard_layout(const char *layout, void *ctx)
{
    (void)ctx;
    esp_err_t err = jetkvm_settings_set_keyboard_layout(layout);
    if (err == ESP_OK) {
        return JETKVM_KEYBOARD_LAYOUT_CHANGE_ACCEPTED;
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return JETKVM_KEYBOARD_LAYOUT_CHANGE_INVALID;
    }
    return JETKVM_KEYBOARD_LAYOUT_CHANGE_FAILED;
}

static bool settings_rpc_get_edid_preset(size_t index, const char **name,
                                         char *edid, size_t edid_capacity,
                                         void *ctx)
{
    (void)ctx;
    size_t count = 0U;
    const p4_edid_descriptor_t *presets = p4_edid_presets(&count);
    *name = NULL;
    if (index >= count) {
        return true;
    }
    *name = presets[index].label;
    return p4_edid_to_hex(&presets[index], edid, edid_capacity);
}

static bool settings_rpc_get_edid(char *output, size_t output_capacity,
                                  void *ctx)
{
    (void)ctx;
    if (!p4_edid_control_lock(100)) {
        return false;
    }
    jetkvm_video_edid_preset_t preset = JETKVM_DEFAULT_VIDEO_EDID_PRESET;
    esp_err_t error = jetkvm_settings_get_video_edid_preset(&preset);
    const p4_edid_descriptor_t *descriptor = p4_edid_descriptor(preset);
    bool success = error == ESP_OK && descriptor &&
                   p4_edid_to_hex(descriptor, output, output_capacity);
    p4_edid_control_unlock();
    return success;
}

/* setEDID persists the preset before the capture owner programs the bridge.
 * When that apply fails the owner falls back to the previous hardware EDID,
 * so the persisted preset must follow, or getEDID reports a preset the bridge
 * does not use and every boot retries it. Persisting writes flash, which
 * needs an internal-RAM stack, so it runs on its own short-lived task. */
static void edid_persist_task(void *argument)
{
    const uintptr_t packed = (uintptr_t)argument;
    const jetkvm_video_edid_preset_t requested =
        (jetkvm_video_edid_preset_t)(packed >> 8);
    const jetkvm_video_edid_preset_t applied =
        (jetkvm_video_edid_preset_t)(packed & 0xFFU);
    /* Serialise with setEDID: a newer choice may already be persisted and
     * queued, and it must not be replaced by this rollback. */
    if (!p4_edid_control_lock(1000)) {
        ESP_LOGE(TAG, "EDID preset restore skipped: control busy");
        vTaskDelete(NULL);
    }
    jetkvm_video_edid_preset_t persisted = JETKVM_DEFAULT_VIDEO_EDID_PRESET;
    if (jetkvm_settings_get_video_edid_preset(&persisted) == ESP_OK &&
        persisted == requested) {
        esp_err_t error = jetkvm_settings_set_video_edid_preset(applied);
        ESP_LOGW(TAG, "EDID preset persisted back to %u after a failed apply: %s",
                 (unsigned)applied, esp_err_to_name(error));
    }
    p4_edid_control_unlock();
    vTaskDelete(NULL);
}

static void edid_apply_result(jetkvm_video_edid_preset_t requested,
                              jetkvm_video_edid_preset_t applied,
                              esp_err_t error, void *ctx)
{
    (void)ctx;
    /* A newer setEDID queued during this apply decides the preset; its own
     * result persists the outcome, so this failure must not roll back. */
    if (error == ESP_OK || requested == applied || p4_edid_control_pending()) {
        return;
    }
    const uintptr_t packed = ((uintptr_t)requested << 8) | (uintptr_t)applied;
    if (xTaskCreate(edid_persist_task, "edid_persist", 4096, (void *)packed,
                    2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "EDID preset restore task unavailable");
    }
}

static jetkvm_edid_change_result_t settings_rpc_set_edid(const char *edid,
                                                          void *ctx)
{
    (void)ctx;
    const p4_edid_descriptor_t *target = NULL;
    size_t edid_preset_count = 0;
    const p4_edid_descriptor_t *edid_presets =
        p4_edid_presets(&edid_preset_count);
    for (size_t i = 0U; i < edid_preset_count; ++i) {
        if (p4_edid_hex_matches(edid, &edid_presets[i])) {
            target = &edid_presets[i];
            break;
        }
    }
    if (!target) {
        return JETKVM_EDID_CHANGE_INVALID;
    }
    if (!p4_edid_control_ready()) {
        return JETKVM_EDID_CHANGE_UNAVAILABLE;
    }
    if (!p4_edid_control_lock(100)) {
        return JETKVM_EDID_CHANGE_BUSY;
    }

    jetkvm_video_edid_preset_t previous =
        JETKVM_DEFAULT_VIDEO_EDID_PRESET;
    esp_err_t error = jetkvm_settings_get_video_edid_preset(&previous);
    if (error != ESP_OK) {
        p4_edid_control_unlock();
        return JETKVM_EDID_CHANGE_UNAVAILABLE;
    }
    jetkvm_video_edid_preset_t applied = p4_edid_control_applied_preset();
    if (target->preset == previous && target->preset == applied) {
        p4_edid_control_unlock();
        return JETKVM_EDID_CHANGE_ACCEPTED;
    }

    /* This runs on the WebRTC peer task, which also services SCTP, DTLS and
     * ICE.  Waiting for the bridge to reprogram stalls the session the reply
     * must travel on, so persist the choice, queue the hardware change and answer at
     * once.  Upstream returns as soon as the EDID file is written too; the UI
     * confirms through getEDID, which reports the persisted preset. */
    error = jetkvm_settings_set_video_edid_preset(target->preset);
    if (error != ESP_OK) {
        p4_edid_control_unlock();
        return JETKVM_EDID_CHANGE_FAILED;
    }
    error = p4_edid_submit_async_locked(target->preset);
    if (error != ESP_OK) {
        esp_err_t restore_error =
            jetkvm_settings_set_video_edid_preset(previous);
        if (restore_error != ESP_OK) {
            ESP_LOGE(TAG, "EDID preset restore failed: %s",
                     esp_err_to_name(restore_error));
        }
        p4_edid_control_unlock();
        return error == ESP_ERR_INVALID_STATE ? JETKVM_EDID_CHANGE_UNAVAILABLE :
               error == ESP_ERR_INVALID_RESPONSE ? JETKVM_EDID_CHANGE_BUSY :
                                                   JETKVM_EDID_CHANGE_FAILED;
    }

    p4_edid_control_unlock();
    return JETKVM_EDID_CHANGE_ACCEPTED;
}

static bool settings_rpc_get_host_display_idle(bool *enabled, void *ctx)
{
    (void)ctx;
    return jetkvm_settings_get_host_display_idle(enabled) == ESP_OK;
}

static jetkvm_host_display_idle_change_result_t
settings_rpc_set_host_display_idle(bool enabled, void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_settings_set_host_display_idle(enabled);
    if (error != ESP_OK) {
        return error == ESP_ERR_INVALID_STATE ?
            JETKVM_HOST_DISPLAY_IDLE_CHANGE_UNAVAILABLE :
            JETKVM_HOST_DISPLAY_IDLE_CHANGE_FAILED;
    }
    jetkvm_video_set_host_display_idle_enabled(enabled);
    return JETKVM_HOST_DISPLAY_IDLE_CHANGE_ACCEPTED;
}

static bool settings_rpc_get_video_sleep(bool *enabled,
                                         int32_t *duration_seconds,
                                         void *ctx)
{
    (void)ctx;
    if (!enabled || !duration_seconds ||
        jetkvm_settings_get_video_sleep_after(duration_seconds) != ESP_OK) {
        return false;
    }
    *enabled = jetkvm_video_is_sleep_active();
    return true;
}

static jetkvm_video_sleep_change_result_t
settings_rpc_set_video_sleep(int32_t duration_seconds, void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_settings_set_video_sleep_after(duration_seconds);
    if (error != ESP_OK) {
        if (error == ESP_ERR_INVALID_ARG) {
            return JETKVM_VIDEO_SLEEP_CHANGE_INVALID;
        }
        return error == ESP_ERR_INVALID_STATE ?
            JETKVM_VIDEO_SLEEP_CHANGE_UNAVAILABLE :
            JETKVM_VIDEO_SLEEP_CHANGE_FAILED;
    }
    jetkvm_video_set_sleep_after_seconds(duration_seconds);
    return JETKVM_VIDEO_SLEEP_CHANGE_ACCEPTED;
}

/* Runs on the OTA task: render the current state and hand it to the
 * transport's peer-task notification queue.  Best-effort by design. */
#define WEBRTC_OTA_NOTIFY_EVENT_CAPACITY 1536U

static void ota_state_listener(void *ctx)
{
    (void)ctx;
    char event[WEBRTC_OTA_NOTIFY_EVENT_CAPACITY];
    size_t event_size = 0;
    if (!jetkvm_ota_render_current_state_event(event, sizeof(event),
                                               &event_size)) {
        return;
    }
    (void)webrtc_transport_send_rpc_notification((const uint8_t *)event,
                                                 event_size);
}

static bool ota_session_active(void *ctx)
{
    (void)ctx;
    webrtc_transport_stats_t stats;
    if (webrtc_transport_get_stats(&stats) != ESP_OK) {
        return true;
    }
    return stats.browser_session_active || stats.peer_connected;
}

static void ota_video_pause(bool paused, void *ctx)
{
    (void)ctx;
    /* Suspend capture too, not just encode: see jetkvm_video_set_capture_suspended. */
    jetkvm_video_set_capture_suspended(paused);
    esp_err_t err =
        webrtc_transport_set_video_paused_by(WEBRTC_VIDEO_PAUSE_OTA, paused);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "video %s for OTA failed: %s",
                 paused ? "pause" : "resume", esp_err_to_name(err));
    }
}

static void storage_download_copy_state(
    jetkvm_control_storage_download_state_t *output)
{
    jetkvm_storage_download_state_t state;
    jetkvm_storage_download_get_state(&state);
    _Static_assert(sizeof(output->filename) == sizeof(state.filename),
                   "download filename capacity");
    _Static_assert(sizeof(output->error) == sizeof(state.error),
                   "download error capacity");
    *output = (jetkvm_control_storage_download_state_t){
        .phase = (jetkvm_control_storage_download_phase_t)state.phase,
        .bytes = state.bytes,
        .total = state.total,
        .reused = state.reused,
        .video_paused = state.video_paused,
    };
    memcpy(output->filename, state.filename, sizeof(output->filename));
    memcpy(output->error, state.error, sizeof(output->error));
}

/* Runs on the download task, as ota_state_listener runs on the OTA task. */
static void storage_download_listener(void *ctx)
{
    (void)ctx;
    jetkvm_control_storage_download_state_t state;
    storage_download_copy_state(&state);
    char event[768];
    size_t event_size = 0;
    if (jetkvm_control_render_storage_download_event(&state, event,
                                                     sizeof(event),
                                                     &event_size)) {
        (void)webrtc_transport_send_rpc_notification((const uint8_t *)event,
                                                     event_size);
    }
}

/* The transfer stops frame delivery only: capture and encode keep running,
 * so video resumes with a fresh IDR as soon as the file is on the card. */
static void storage_download_video_pause(bool paused, void *ctx)
{
    (void)ctx;
    esp_err_t err = webrtc_transport_set_video_paused_by(
        WEBRTC_VIDEO_PAUSE_STORAGE_DOWNLOAD, paused);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "video %s for the storage download failed: %s",
                 paused ? "pause" : "resume", esp_err_to_name(err));
    }
}

/* 1: a browser upload to the TF card holds video off for as long as it holds
 * the card's writer lease. On the Wi-Fi Mini the upload and video share the
 * radio link, and video does not get through beside an upload. The host's
 * sound stops with video (WEBRTC_AUDIO_PAUSE_SOURCES). The Ethernet Mini
 * keeps video and sound during an upload. */
#define JETKVM_UPLOAD_PAUSES_VIDEO JETKVM_BOARD_WIFI

/* Called with the virtual media lock held. As for the download, only frame
 * delivery stops, and video resumes with a fresh IDR. */
static void storage_upload_video_pause(bool paused, void *ctx)
{
    (void)ctx;
    esp_err_t err = webrtc_transport_set_video_paused_by(
        WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD, paused);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "video %s for the storage upload failed: %s",
                 paused ? "pause" : "resume", esp_err_to_name(err));
    }
}

static bool storage_download_start(const char *url, const char *filename,
                                   char *error, size_t error_capacity,
                                   void *ctx)
{
    (void)ctx;
    return jetkvm_storage_download_start(url, filename, error,
                                         error_capacity) == ESP_OK;
}

static bool storage_download_get_state(
    jetkvm_control_storage_download_state_t *state, void *ctx)
{
    (void)ctx;
    storage_download_copy_state(state);
    return true;
}

/* The live cloud WebSocket, not the registration getCloudState reports. */
static bool cloud_link(webrtc_transport_cloud_link_t *link)
{
    jetkvm_cloud_service_stats_t stats;
    if (jetkvm_cloud_service_get_stats(&stats) != ESP_OK) {
        return false;
    }
    *link = (webrtc_transport_cloud_link_t) {
        .connected = stats.client.connected,
        .connection_epoch = stats.client.connection_epoch,
        .client_rebuilds = stats.rebuilds,
    };
    return true;
}

/* Delay between the willReboot notification and the reset, as in Go.  The
 * browser's rebooting overlay starts its 30 s reconnect timer at willReboot,
 * and the Wi-Fi board needs ~18 s from reset to a routable address, so the
 * delay has to stay well inside the remaining budget. */
#define OTA_REBOOT_DELAY_MS 5000U

static void ota_reboot(const char *redirect_to, void *ctx)
{
    (void)ctx;
    char event[192];
    int length = snprintf(
        event, sizeof(event),
        "{\"jsonrpc\":\"2.0\",\"method\":\"willReboot\",\"params\":"
        "{\"healthCheck\":\"/device/status\",\"redirectTo\":\"%s\"}}",
        redirect_to != NULL ? redirect_to : "/");
    if (length > 0 && (size_t)length < sizeof(event)) {
        (void)webrtc_transport_send_rpc_notification((const uint8_t *)event,
                                                     (size_t)length);
    }
    /* Let the notification drain and the browser settle into the rebooting
     * overlay before the reset drops the SCTP association.  The overlay polls
     * the health check only after it has seen the peer disconnect, so the
     * delay cannot make it find the still-up web pack. */
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}

/* A freshly OTA-booted image proves itself by reaching full userspace and
 * surviving the first capture seconds; a panic before the confirmation lets
 * the bootloader restore the previous image on the next reset. */
#define OTA_CONFIRM_DELAY_MS 10000U

static void ota_confirm_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_CONFIRM_DELAY_MS));
    jetkvm_ota_confirm_running_image();
    vTaskDelete(NULL);
}


/* Upstream attemptHidWriteRecovery rebinds the UDC when keyboard writes keep
 * timing out with the host still configured (a wedged host HID driver).  The
 * reconnect waits for the HID worker, so it runs on its own short-lived task. */
static void hid_write_recovery_task(void *argument)
{
    (void)argument;
#if CONFIG_JETKVM_OS_SERVICES
    jetkvm_os_services_usb_recovery_next();
#endif
    jetkvm_usb_service_configuration_result_t result =
        jetkvm_usb_service_reenumerate();
#if CONFIG_JETKVM_OS_SERVICES
    jetkvm_os_services_usb_recovery_done();
#endif
    ESP_LOGW(TAG, "USB gadget reconnect after keyboard write timeouts: %d",
             (int)result);
    vTaskDelete(NULL);
}

static void hid_write_recovery_requested(unsigned consecutive_timeouts,
                                         void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG,
             "keyboard HID writes timed out %u times while USB is configured; reconnecting gadget",
             consecutive_timeouts);
    if (xTaskCreate(hid_write_recovery_task, "usb_recover", 4096, NULL, 2,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "USB gadget reconnect task unavailable");
    }
}

#if CONFIG_JETKVM_OS_SERVICES
static void os_services_state_changed(void *ctx)
{
    (void)ctx;
    webrtc_transport_video_state_changed();
}

/* From the task that changed the settings: the peer task's RPC dispatch. */
static void os_services_settings_changed(void *ctx)
{
    (void)ctx;
    const size_t capacity = 1280U;
    char *event = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t length = 0;
    if (event != NULL &&
        jetkvm_os_services_render_settings_event(event, capacity, &length)) {
        (void)webrtc_transport_send_rpc_notification((const uint8_t *)event,
                                                     length);
    }
    heap_caps_free(event);
}

/* From any task: the terminal and file channels have output for the peer
 * task to send. */
static void os_services_channel_output(void *ctx)
{
    (void)ctx;
    webrtc_transport_guest_output_pending();
}

static bool os_services_next_output(webrtc_transport_guest_output_t *output)
{
    jetkvm_os_services_channel_output_t next;
    if (!jetkvm_os_services_channel_next_output(&next)) {
        return false;
    }
    *output = (webrtc_transport_guest_output_t){
        .channel = next.channel,
        .close = next.close,
        .text = next.text,
        .data = next.data,
        .length = next.length,
    };
    return true;
}

/* Before TinyUSB is installed: the OS Services class driver must exist
 * when the host configures the device. */
static void start_os_services(void)
{
    esp_err_t error = jetkvm_os_services_init();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "OS Services degraded: %s", esp_err_to_name(error));
    }
    if (!jetkvm_os_services_is_started()) {
        return;
    }
    const jetkvm_os_services_listener_t listener = {
        .state_changed = os_services_state_changed,
        .settings_changed = os_services_settings_changed,
        .channel_output = os_services_channel_output,
    };
    jetkvm_os_services_set_listener(&listener);
    const jetkvm_os_services_context_t context = {
        .get_settings = jetkvm_os_services_get_settings_json,
        .set_settings = jetkvm_os_services_set_settings_json,
        .add_video_state = jetkvm_os_services_add_video_state,
        .update_start = jetkvm_os_services_update_start,
    };
    jetkvm_control_set_default_os_services_context(&context);
    const jetkvm_guest_clipboard_context_t clipboard = {
        .get = jetkvm_os_services_clipboard_get,
        .set = jetkvm_os_services_clipboard_set,
    };
    jetkvm_control_set_default_guest_clipboard_context(&clipboard);
    const jetkvm_guest_file_context_t file = {
        .drop = jetkvm_os_services_file_drop,
    };
    jetkvm_control_set_default_guest_file_context(&file);
    /* The browser's guest-terminal and guestfile_<drop> channels. */
    const webrtc_transport_guest_channels_t channels = {
        .claim = jetkvm_os_services_channel_claim,
        .message = jetkvm_os_services_channel_message,
        .closed = jetkvm_os_services_channel_closed,
        .next_output = os_services_next_output,
        .output_done = jetkvm_os_services_channel_output_done,
    };
    webrtc_transport_set_guest_channels(&channels);
}
#endif

static void usb_service_state_changed(
    const jetkvm_usb_service_snapshot_t *snapshot, void *ctx)
{
    (void)ctx;
    if (snapshot == NULL) {
        return;
    }
    /* Capability describes protocol support, not the user's current target
     * USB enable switch.  The bridge stays initialized across normal off/on
     * transitions, so a replacement browser can establish its
     * generation-gated HIDRPC channel while usbAvailable/acceptingInput still
     * reject every physical report.  Querying the bridge also avoids
     * advertising HID if the owner's first initialization never succeeded. */
    jetkvm_hid_input_snapshot_t hid_input = {0};
    bool hid_protocol_ready =
        jetkvm_hid_input_get_snapshot(&hid_input) == ESP_OK &&
        hid_input.initialized;
    p4_device_set_hid_available(hid_protocol_ready);
    /* Remote wake is implemented by the same persistent bridge.  Keep the
     * capability stable while target USB is intentionally disabled; the RPC
     * adapter returns JetKVM's no-op result until a suspended host exists. */
    p4_device_set_usb_wake_available(hid_protocol_ready);
    p4_device_set_usb_configuration_available(
        snapshot->initialized && snapshot->configuration_ready);
}

#if USB_MSC_BENCHMARK_AUTOMOUNT_FIRST_IMAGE
static bool benchmark_filename_has_suffix(const char *filename,
                                          const char *suffix)
{
    const size_t filename_length = strlen(filename);
    const size_t suffix_length = strlen(suffix);
    return filename_length >= suffix_length &&
           strcasecmp(filename + filename_length - suffix_length, suffix) == 0;
}

static esp_err_t benchmark_automount_largest_tf_image(void)
{
    jetkvm_virtual_media_file_list_t *files = heap_caps_calloc(
        1U, sizeof(*files), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (files == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = jetkvm_virtual_media_list_files(files);
    if (error != ESP_OK) {
        heap_caps_free(files);
        return error;
    }

    const jetkvm_virtual_media_file_t *selected = NULL;
    ESP_LOGI(TAG, "USB_MSC_BENCH: TF images=%u truncated=%d",
             (unsigned)files->count, files->truncated);
    for (size_t i = 0; i < files->count; ++i) {
        const jetkvm_virtual_media_file_t *file = &files->files[i];
        ESP_LOGI(TAG, "USB_MSC_BENCH: image[%u]=%s bytes=%" PRIu64,
                 (unsigned)i, file->filename, file->size);
        if (file->size == 0U ||
            benchmark_filename_has_suffix(file->filename, ".incomplete")) {
            continue;
        }
        if (selected == NULL || file->size > selected->size) {
            selected = file;
        }
    }
    if (selected == NULL) {
        heap_caps_free(files);
        return ESP_ERR_NOT_FOUND;
    }

    const jetkvm_virtual_media_mode_t mode =
        benchmark_filename_has_suffix(selected->filename, ".iso") ?
            JETKVM_VIRTUAL_MEDIA_MODE_CDROM :
            JETKVM_VIRTUAL_MEDIA_MODE_DISK;
    error = jetkvm_virtual_media_mount(selected->filename, mode);
    if (error == ESP_OK) {
        ESP_LOGW(TAG,
                 "USB_MSC_BENCH: automounted %s bytes=%" PRIu64 " mode=%s",
                 selected->filename, selected->size,
                 mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ? "cdrom" : "disk");
    }
    heap_caps_free(files);
    return error;
}
#endif

/* Runs inside esp_restart before the reset; names the task that asked. */
static void log_restart_origin(void)
{
    ESP_LOGW(TAG, "restart requested by task %s", pcTaskGetName(NULL));
}

/*
 * The EMAC and the SDMMC host take their resets from LP_CLKRST, outside the
 * HP system. IDF 6.0.2's restart leaves the EMAC running (espressif/esp-idf
 * #19085, fixed upstream after 6.0.2), and a receive left running writes
 * frames, through the last boot's descriptors, into memory the next boot
 * fills with code and heap. The digital-core reset below does not reset
 * them (it leaves HP_SYS_CLKRST and LP_CLKRST alone), so reset both before
 * it. The start of startup resets them as well, for a reset that comes
 * without this path, such as a watchdog's. The stock bootloader does not
 * touch them. In IRAM: the restart path runs it with the flash cache
 * disabled.
 */
static inline IRAM_ATTR void reset_lp_clocked_dma(void)
{
    SET_PERI_REG_MASK(LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG,
                      LP_CLKRST_RST_EN_EMAC | LP_CLKRST_RST_EN_SDMMC);
    CLEAR_PERI_REG_MASK(LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG,
                        LP_CLKRST_RST_EN_EMAC | LP_CLKRST_RST_EN_SDMMC);
}

/* Before the heap is set up, so a frame written from here on cannot land in
 * an allocation. */
ESP_SYSTEM_INIT_FN(reset_lp_clocked_dma_at_boot, CORE, BIT(0), 0)
{
    reset_lp_clocked_dma();
    return ESP_OK;
}

/* esp_restart_noos resets only the CPUs and some peripherals; the video
 * capture hardware keeps writing to PSRAM, and the next boot can hang in its
 * PSRAM memory test until the bootloader watchdog fires. Reset the whole
 * digital core, as that watchdog does. main/CMakeLists.txt wraps the call.
 * In IRAM: a panic calls it with the flash cache disabled. A panic also
 * stalls the other core, and that stall outlives a digital-core reset. */
void IRAM_ATTR __wrap_esp_restart_noos(void)
{
    esp_cpu_unstall(esp_cpu_get_core_id() == 0 ? 1 : 0);
    reset_lp_clocked_dma();
    esp_rom_software_reset_system();
    for (;;) {
    }
}

#if JETKVM_BOARD_WIFI
static const char *wifi_setup_from_display(bool start)
{
    return start ? jetkvm_c5_wifi_start_setup() : jetkvm_c5_wifi_stop_setup();
}
#endif

static void show_reset_progress(void)
{
    esp_err_t error = jetkvm_board_network_screen_start();
    if (error != ESP_OK)
        ESP_LOGW(TAG, "Reset display unavailable: %s", esp_err_to_name(error));
}

static void flush_console(void)
{
    jetkvm_console_flush();
}

void app_main(void)
{
    /* Before anything logs from a task that must not wait for the console.
     * Shutdown handlers run last-registered first, so the flush registered
     * here runs after log_restart_origin has queued its line. */
    if (jetkvm_console_start() == ESP_OK) {
        (void)esp_register_shutdown_handler(flush_console);
    }
    (void)esp_register_shutdown_handler(log_restart_origin);
    /* First, so that the rest of the start already fetches its code from
     * flash in the faster mode, and before another task uses the cache. */
    jetkvm_flash_read_mode_start();
#if P4_STAGED_DIAGNOSTIC
    p4_staged_diagnostic();
    return;
#endif
#if JETKVM_BOARD_WIFI
    ESP_LOGI(TAG, "Board variant: wifi (FPC2 screen + ESP32-C5 SDIO)");
#else
    ESP_LOGI(TAG, "Board variant: ethernet");
#endif

    /* Carve the bounded control-plane heap and install process-wide cJSON
     * hooks before any service can build an object graph or launch a worker.
     * Video requires PSRAM anyway, so a missing external heap is a coherent
     * boot failure rather than fallback into scarce internal/DMA SRAM. */
    ESP_ERROR_CHECK(jetkvm_memory_policy_init());
    /* The video pool is the largest PSRAM owner. Allocating it before the
     * other long-lived owners keeps it contiguous and outside their holes. */
    ESP_ERROR_CHECK(jetkvm_video_reserve_buffers());
    p4_edid_set_apply_result_handler(edid_apply_result, NULL);
    esp_err_t ret = nvs_flash_init();

    /* Never repair unreadable NVS by erasing it: it can contain credentials
     * and a pending reset intent. Preserve it for recovery instead of losing
     * the marker before internal-storage/radio cleanup has completed. */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); data preserved, serial recovery required",
                 esp_err_to_name(ret));
        return; /* Keep the independent USB Serial/JTAG recovery path alive. */
    }
    ret = jetkvm_factory_reset_boot_begin(show_reset_progress);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset incomplete; reboot to retry: %s", esp_err_to_name(ret));
        return; /* Keep the reset error visible; do not start provisioning. */
    }
    /* USB debugging: off unless a user turned it on (on in a bench build).
     * A normal boot applies it here, before Wi-Fi or any other service can
     * wait. A boot with a pending factory reset applies it once the reset
     * has finished: a reset that fails returns with the port as it is for
     * recovery, and a finished reset has erased the setting, so the start
     * uses the build default. */
    const bool reset_pending = jetkvm_factory_reset_pending();
    if (!reset_pending) {
        jetkvm_usb_debugging_start();
    }
    /* Show the boot logo before radio and storage initialization can block.
     * Read the reset intent first so pending resets use display defaults. */
    esp_err_t screen_error = jetkvm_board_network_screen_start();
    if (screen_error != ESP_OK) {
        ESP_LOGW(TAG, "JetKVM screen unavailable; local video continues: %s",
                 esp_err_to_name(screen_error));
    }

#if JETKVM_BOARD_WIFI
    /* Keep Hosted on SDMMC slot 1 before mounting the TF card on slot 0. This
     * is the initialization order used by Espressif's shared-SDMMC example. */
    ret = jetkvm_c5_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP32-C5 Wi-Fi startup failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "ESP32-C5 Wi-Fi startup passed");
        jetkvm_screen_set_wifi_setup_handler(wifi_setup_from_display);
    }
#endif

    /* Wi-Fi finishes the reset before provisioning. Publish a radio failure,
     * or finish here on Ethernet, which has no radio credentials to erase. */
    if (jetkvm_factory_reset_pending() &&
        jetkvm_factory_reset_finish(ret) != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset incomplete; reboot to retry");
        return;
    }
    if (reset_pending) {
        jetkvm_usb_debugging_start();
    }

    p4_device_set_wake_on_lan_available(false);
    ret = jetkvm_wol_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wake-on-LAN device list recovered empty: %s",
                 esp_err_to_name(ret));
    }
    if (jetkvm_wol_is_initialized()) {
        p4_device_set_wake_on_lan_available(true);
    }

    p4_device_set_virtual_media_http_available(false);
#if P4_SKIP_TF_DIAGNOSTIC
    ESP_LOGW(TAG, "P4_DIAG TF/SDMMC initialization disabled");
#else
    ret = jetkvm_virtual_media_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TF virtual-media storage unavailable: %s",
                 esp_err_to_name(ret));
    } else {
        p4_device_set_virtual_media_http_available(true);
#if USB_MSC_BENCHMARK_AUTOMOUNT_FIRST_IMAGE
        esp_err_t benchmark_mount_error =
            benchmark_automount_largest_tf_image();
        if (benchmark_mount_error != ESP_OK) {
            ESP_LOGW(TAG, "USB_MSC_BENCH: automount failed: %s",
                     esp_err_to_name(benchmark_mount_error));
        }
#endif
    }
#endif

    p4_device_set_keyboard_layout_available(false);
    p4_device_set_edid_control_available(false);
    p4_device_set_host_display_idle_control_available(false);
    ret = jetkvm_settings_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "persistent settings loaded with safe defaults: %s",
                 esp_err_to_name(ret));
    }
    if (jetkvm_settings_is_initialized()) {
        double video_quality = 1.0;
        if (jetkvm_settings_get_video_quality(&video_quality) == ESP_OK &&
            !jetkvm_video_quality_set(video_quality)) {
            ESP_LOGW(TAG, "stored video quality %.3f is invalid; using Auto",
                     video_quality);
            (void)jetkvm_video_quality_set(
                (double)JETKVM_VIDEO_QUALITY_DEFAULT_PERMILLE / 1000.0);
        }
        const jetkvm_keyboard_layout_context_t keyboard_layout_context = {
            .get = settings_rpc_get_keyboard_layout,
            .set = settings_rpc_set_keyboard_layout,
        };
        jetkvm_control_set_default_keyboard_layout_context(
            &keyboard_layout_context);
        p4_device_set_keyboard_layout_available(true);

        bool host_display_idle_enabled = false;
        if (jetkvm_settings_get_host_display_idle(
                &host_display_idle_enabled) != ESP_OK) {
            ESP_LOGW(TAG,
                     "host display idle setting unavailable; leaving HPD visible");
            host_display_idle_enabled = false;
        }
        jetkvm_video_set_host_display_idle_enabled(
            host_display_idle_enabled);
        const jetkvm_host_display_idle_context_t host_display_idle_context = {
            .get = settings_rpc_get_host_display_idle,
            .set = settings_rpc_set_host_display_idle,
        };
        jetkvm_control_set_default_host_display_idle_context(
            &host_display_idle_context);
        p4_device_set_host_display_idle_control_available(true);

        int32_t video_sleep_after_seconds =
            JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS;
        if (jetkvm_settings_get_video_sleep_after(
                &video_sleep_after_seconds) != ESP_OK) {
            ESP_LOGW(TAG,
                     "video sleep setting unavailable; leaving capture awake");
            video_sleep_after_seconds =
                JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS;
        }
        jetkvm_video_set_sleep_after_seconds(video_sleep_after_seconds);
        const jetkvm_video_sleep_context_t video_sleep_context = {
            .get = settings_rpc_get_video_sleep,
            .set = settings_rpc_set_video_sleep,
        };
        jetkvm_control_set_default_video_sleep_context(
            &video_sleep_context);
        p4_device_set_video_sleep_control_available(true);

        esp_err_t edid_control_error = p4_edid_control_init();
        if (edid_control_error != ESP_OK) {
            ESP_LOGW(TAG, "EDID command boundary unavailable: %s",
                     esp_err_to_name(edid_control_error));
        } else {
            const jetkvm_edid_context_t edid_context = {
                .get_preset = settings_rpc_get_edid_preset,
                .get = settings_rpc_get_edid,
                .set = settings_rpc_set_edid,
            };
            jetkvm_control_set_default_edid_context(&edid_context);
            /* Publish before the HTTP service starts so a browser cannot
             * cache a transient false profile during boot.  EDID RPC itself
             * is carried by a peer that only becomes usable after capture
             * initialization marks the owner ready. */
            p4_device_set_edid_control_available(true);
        }
    }

    p4_device_set_hid_macros_available(false);
    p4_device_set_mouse_jiggler_available(false);
    ret = jetkvm_hid_profiles_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "HID profiles loaded with safe defaults: %s",
                 esp_err_to_name(ret));
    }
    if (jetkvm_hid_profiles_is_initialized()) {
        p4_device_set_hid_macros_available(true);
    }

    ret = jetkvm_device_state_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "cloud/device state loaded with safe defaults: %s",
                 esp_err_to_name(ret));
    }
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    if (jetkvm_device_state_get_id(device_id, sizeof(device_id)) == ESP_OK) {
        ESP_LOGI(TAG, "JetKVM device id=%s", device_id);
    }

    p4_device_set_mqtt_available(false);
    ret = jetkvm_mqtt_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MQTT settings loaded with safe defaults: %s",
                 esp_err_to_name(ret));
    }
    if (jetkvm_mqtt_is_initialized()) {
        jetkvm_mqtt_context_t mqtt_context;
        jetkvm_mqtt_fill_control_context(&mqtt_context);
        jetkvm_control_set_default_mqtt_context(&mqtt_context);
        p4_device_set_mqtt_available(true);
    }

    /* The owner task is the sole lifecycle authority for the P4 USB2 HS/UTMI
     * target controller and its HID bridge. Browser/RPC tasks can only queue
     * commands; they never uninstall TinyUSB themselves. A target USB failure
     * must never take video or the independent USB Serial/JTAG console down. */
    p4_device_set_hid_available(false);
    p4_device_set_usb_wake_available(false);
    p4_device_set_usb_configuration_available(false);
#if !P4_SKIP_TARGET_USB_DIAGNOSTIC
    ret = jetkvm_audio_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Optional audio unavailable: %s", esp_err_to_name(ret));
    }
#if CONFIG_JETKVM_OS_SERVICES
    start_os_services();
#endif
    ret = jetkvm_usb_service_start(usb_service_state_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "HS USB owner start incomplete; video/debug continue: %s",
                 esp_err_to_name(ret));
    } else {
        ESP_ERROR_CHECK(jetkvm_hid_input_set_write_recovery_handler(
            hid_write_recovery_requested, NULL));
        ESP_ERROR_CHECK(jetkvm_hid_input_set_keyboard_changed_handler(
            webrtc_transport_keyboard_changed, NULL));
        jetkvm_usb_service_snapshot_t usb_service = {0};
        ESP_ERROR_CHECK(jetkvm_usb_service_get_snapshot(&usb_service));
        ESP_LOGI(TAG,
                 "JetKVM target USB owner ready driver=%d hid=%d desired=%d (target mount is optional at boot)",
                 usb_service.driver_enabled, usb_service.hid_input_ready,
                 usb_service.desired_enabled);
    }
#else
    ESP_LOGW(TAG, "P4_DIAG target USB disabled; isolating PPA/SPI stability");
#endif
    if (jetkvm_hid_profiles_is_initialized()) {
        esp_err_t jiggler_error = jetkvm_hid_profiles_start_jiggler();
        if (jiggler_error == ESP_OK) {
            p4_device_set_mouse_jiggler_available(true);
        } else {
            ESP_LOGW(TAG, "mouse jiggler scheduler unavailable: %s",
                     esp_err_to_name(jiggler_error));
        }
    }

    jetkvm_oidc_config_t oidc_config;
    jetkvm_oidc_default_config(&oidc_config);
    jetkvm_oidc_status_t oidc_status = jetkvm_oidc_validator_create(
        &oidc_config, &s_cloud_oidc_validator);
    if (oidc_status != JETKVM_OIDC_OK) {
        ESP_LOGW(TAG, "cloud OIDC validator unavailable: %s",
                 jetkvm_oidc_status_name(oidc_status));
        s_cloud_oidc_validator = NULL;
    } else {
        ret = p4_device_api_set_oidc_validator(s_cloud_oidc_validator);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "cloud registration API unavailable: %s",
                     esp_err_to_name(ret));
            jetkvm_oidc_validator_destroy(s_cloud_oidc_validator);
            s_cloud_oidc_validator = NULL;
        }
    }

#if JETKVM_BOARD_ETHERNET
    jetkvm_board_network_ethernet_start();
#else
    ESP_LOGI(TAG, "Ethernet startup skipped on Wi-Fi board variant");
#endif
    ret = jetkvm_time_sync_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "cloud time sync start failed: %s",
                 esp_err_to_name(ret));
    }
    webrtc_transport_set_cloud_link_source(cloud_link);
    ret = jetkvm_tailscale_init();
    if (ret == ESP_OK) {
        jetkvm_tailscale_fill_control_context(&s_tailscale_context);
        jetkvm_control_set_default_tailscale_context(&s_tailscale_context);
    } else {
        ESP_LOGW(TAG, "Tailscale unavailable: %s", esp_err_to_name(ret));
    }
    webrtc_transport_set_video_encode_source(jetkvm_video_get_encode_stats);
    webrtc_transport_set_hdmi_receiver_source(
        jetkvm_video_get_hdmi_receiver_stats);
    /* The transport's start also starts the HTTP server, which takes
     * uploads, so the hook is in place before it. */
    if (JETKVM_UPLOAD_PAUSES_VIDEO) {
        jetkvm_virtual_media_set_upload_writer_hook(storage_upload_video_pause,
                                                    NULL);
    }
    jetkvm_access_set_host_names(access_host_names);
    jetkvm_control_set_default_extra_hostnames_context(&(jetkvm_extra_hostnames_context_t){
        .get = extra_hostnames_get, .set = extra_hostnames_set});
    ESP_ERROR_CHECK(webrtc_transport_start());
    if (s_cloud_oidc_validator) {
        ret = jetkvm_cloud_service_start(s_cloud_oidc_validator);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "cloud service unavailable; local video continues: %s",
                     esp_err_to_name(ret));
        }
    }
    if (jetkvm_mqtt_is_initialized()) {
        ret = jetkvm_mqtt_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "MQTT service start deferred; local KVM continues: %s",
                     esp_err_to_name(ret));
        }
    }

    const jetkvm_storage_download_hooks_t storage_download_hooks = {
        .listener = storage_download_listener,
        .video_pause = storage_download_video_pause,
    };
    jetkvm_storage_download_set_hooks(&storage_download_hooks);
    const jetkvm_storage_download_context_t storage_download_context = {
        .start = storage_download_start,
        .get_state = storage_download_get_state,
    };
    jetkvm_control_set_default_storage_download_context(
        &storage_download_context);

    p4_device_set_ota_available(false);
    const jetkvm_ota_hooks_t ota_hooks = {
        .state_listener = ota_state_listener,
        .session_active = ota_session_active,
        .reboot = ota_reboot,
        .video_pause = ota_video_pause,
    };
    ret = jetkvm_ota_start(&ota_hooks);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA update service unavailable; local KVM continues: %s",
                 esp_err_to_name(ret));
    } else {
        jetkvm_ota_context_t ota_context;
        jetkvm_ota_fill_control_context(&ota_context);
        jetkvm_control_set_default_ota_context(&ota_context);
        p4_device_set_ota_available(true);
    }

    jetkvm_board_network_screen_status_start();

#if P4_SKIP_VIDEO_DIAGNOSTIC
    ESP_LOGW(TAG,
             "P4_DIAG full services active; video/CSI/PPA/H264 disabled");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGW(TAG, "P4_DIAG stage=services_no_video uptime=%" PRId64 "s",
                 esp_timer_get_time() / 1000000);
    }
#else
    ESP_ERROR_CHECK(jetkvm_video_start());
#endif

    /* Confirms a pending-verify OTA image once userspace proves stable; a
     * no-op on already-confirmed images. */
    BaseType_t ota_confirm_created = xTaskCreatePinnedToCore(
        ota_confirm_task, "ota_confirm", 3072, NULL, tskIDLE_PRIORITY + 1,
        NULL, 0);
    if (ota_confirm_created != pdPASS) {
        ESP_LOGW(TAG, "OTA confirm task unavailable; an updated image would "
                      "roll back on the next reset");
    }

}
