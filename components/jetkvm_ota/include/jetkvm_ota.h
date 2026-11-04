/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_OTA_VERSION_CAPACITY 32U
#define JETKVM_OTA_URL_CAPACITY 256U
#define JETKVM_OTA_HASH_CAPACITY 65U
#define JETKVM_OTA_ERROR_CAPACITY 232U
#define JETKVM_OTA_TIMESTAMP_CAPACITY 32U
#define JETKVM_OTA_API_URL_CAPACITY 192U
#define JETKVM_OTA_IMAGE_SKU_CAPACITY 32U
/** "JKSK" little-endian: marks a jetkvm_ota_image_desc_t in an image. */
#define JETKVM_OTA_IMAGE_DESC_MAGIC 0x4b534b4aU

/**
 * Custom application descriptor every image carries right after
 * esp_app_desc_t (ESP-IDF's .rodata_custom_desc section).  The installer
 * reads it from the first download chunk and refuses an image built for
 * another board: the ethernet and wireless images are different builds, and
 * rollback only catches an image that fails to boot, not one that boots and
 * drives the wrong pins.  64 bytes; the tail is reserved for later fields.
 */
typedef struct {
    uint32_t magic;
    char sku[JETKVM_OTA_IMAGE_SKU_CAPACITY];
    uint8_t reserved[28];
} jetkvm_ota_image_desc_t;
/** {api_url}/releases plus deviceId, prerelease, sku and both version
 * constraints at their maximum lengths. */
#define JETKVM_OTA_RELEASES_URL_CAPACITY 384U

/**
 * Go-compatible update metadata for one release check.  The release server
 * answers GET {api_url}/releases with a JSON object carrying the remote
 * fields; empty or absent strings mean the component is not part of the
 * offer.  This product has one over-the-air artifact, the firmware image,
 * which the server reports as the system component.  App offers are never
 * acted on.  systemSigUrl names the image's detached GPG signature; the
 * server leaves it out for an image that has none.
 */
typedef struct {
    char app_version[JETKVM_OTA_VERSION_CAPACITY];
    char app_url[JETKVM_OTA_URL_CAPACITY];
    char app_hash[JETKVM_OTA_HASH_CAPACITY];
    char system_version[JETKVM_OTA_VERSION_CAPACITY];
    char system_url[JETKVM_OTA_URL_CAPACITY];
    char system_hash[JETKVM_OTA_HASH_CAPACITY];
    char system_sig_url[JETKVM_OTA_URL_CAPACITY];
} jetkvm_ota_metadata_t;

/** Wire shape of the getUpdateStatus / checkUpdateComponents RPC results. */
typedef struct {
    char local_app_version[JETKVM_OTA_VERSION_CAPACITY];
    char local_system_version[JETKVM_OTA_VERSION_CAPACITY];
    jetkvm_ota_metadata_t remote;
    bool app_update_available;
    bool system_update_available;
    bool will_disable_auto_update;
    char error[JETKVM_OTA_ERROR_CAPACITY];
} jetkvm_ota_update_status_t;

/**
 * Progress snapshot mirroring Go's ota.RPCState.  Timestamps are rendered
 * into the otaState event at push time; a false flag means a null field.
 */
typedef struct {
    bool updating;
    bool metadata_fetched;
    bool app_update_pending;
    float app_download_progress;
    bool app_download_finished;
    float app_verification_progress;
    bool app_verified;
    float app_update_progress;
    bool app_updated;
    bool system_update_pending;
    float system_download_progress;
    bool system_download_finished;
    float system_verification_progress;
    bool system_verified;
    float system_update_progress;
    bool system_updated;
    char error[JETKVM_OTA_ERROR_CAPACITY];
} jetkvm_ota_state_t;

/** Update server a device uses until setUpdateApiUrl changes it. */
#define JETKVM_OTA_DEFAULT_API_URL "https://api.jetkvm.com"

typedef struct {
    char update_api_url[JETKVM_OTA_API_URL_CAPACITY]; /**< never empty */
    bool auto_update_enabled;
    bool include_prerelease;
} jetkvm_ota_config_t;

/** Invoked (from the OTA task) after every state change worth pushing. */
typedef void (*jetkvm_ota_state_listener_fn)(void *ctx);

/** Report whether a browser/WebRTC session is active (auto-update skips). */
typedef bool (*jetkvm_ota_session_active_fn)(void *ctx);

/** Reboot the device after a successful install (main owns esp_restart). */
typedef void (*jetkvm_ota_reboot_fn)(const char *redirect_to, void *ctx);

/**
 * Pause (true) or resume (false) video delivery. Called from the OTA task
 * before the firmware download starts and again only if the install
 * fails; a successful install reboots. Stops frame delivery to viewers,
 * which frees network bandwidth for the download; the encoder keeps
 * running and keeps its memory.
 */
typedef void (*jetkvm_ota_video_pause_fn)(bool paused, void *ctx);

typedef struct {
    jetkvm_ota_state_listener_fn state_listener;
    void *state_listener_ctx;
    jetkvm_ota_session_active_fn session_active;
    void *session_active_ctx;
    jetkvm_ota_reboot_fn reboot;
    void *reboot_ctx;
    jetkvm_ota_video_pause_fn video_pause;
    void *video_pause_ctx;
} jetkvm_ota_hooks_t;

/**
 * The versions a device reports for itself.  The firmware image is the whole
 * system, so the system version is the firmware version and the app version
 * is empty: there is no separate app, and the web UI hides an empty one.
 * Shared by the getLocalVersion RPC, the localVersion event and the update
 * status.
 */
void jetkvm_ota_local_versions(char *app_version, size_t app_capacity,
                               char *system_version, size_t system_capacity);

/** Load NVS config, snapshot local versions, and start the update task. */
esp_err_t jetkvm_ota_start(const jetkvm_ota_hooks_t *hooks);
void jetkvm_ota_stop(void);

esp_err_t jetkvm_ota_get_config(jetkvm_ota_config_t *config);
/** An empty URL restores JETKVM_OTA_DEFAULT_API_URL. */
esp_err_t jetkvm_ota_set_update_api_url(const char *url);
esp_err_t jetkvm_ota_set_auto_update(bool enabled);
esp_err_t jetkvm_ota_set_include_prerelease(bool enabled);

/**
 * Fill the last successful check result plus local versions.  Before the
 * first check the remote fields are empty and both available flags are
 * false, matching Go's initial UpdateStatus.
 */
esp_err_t jetkvm_ota_get_update_status(jetkvm_ota_update_status_t *status);

/**
 * Fetch release metadata now (blocking, network) and update the cached
 * status.  Constraints may be NULL (latest) or an exact version string.
 * Returns ESP_OK when metadata was fetched; check status.error/remote.
 */
esp_err_t jetkvm_ota_check_update(const char *app_constraint,
                                  const char *system_constraint,
                                  bool include_prerelease,
                                  jetkvm_ota_update_status_t *status);

/**
 * Start an asynchronous update: check metadata, download/verify/install the
 * offered components, then reboot through the hook.  Returns
 * ESP_ERR_INVALID_STATE when an update is already running.
 */
esp_err_t jetkvm_ota_try_update(const char *app_constraint,
                                const char *system_constraint,
                                bool include_prerelease,
                                bool reset_config);

bool jetkvm_ota_is_update_pending(void);

/**
 * A download that must not run alongside a firmware install: the OS Services
 * update relay. begin refuses while an install runs. An install that starts
 * calls stop(ctx), from the OTA task, and waits up to 35 seconds for end
 * before it downloads.
 */
bool jetkvm_ota_side_download_begin(void (*stop)(void *ctx), void *ctx);
void jetkvm_ota_side_download_end(void);

/** Whether a browser session is open, as auto-update sees it. */
bool jetkvm_ota_session_active(void);
void jetkvm_ota_snapshot_state(jetkvm_ota_state_t *state);

/**
 * Render the current state as one otaState JSON-RPC notification (with the
 * monotonic timestamp filled in).  The transport glue calls this from the
 * state listener and sends the result on the RPC data channel.
 */
bool jetkvm_ota_render_current_state_event(char *output, size_t capacity,
                                           size_t *out_size);

/**
 * Confirm the running OTA image after the app proves itself healthy.
 * No-op when running from factory or when the image was already confirmed;
 * otherwise calls esp_ota_mark_app_valid_cancel_rollback().
 */
void jetkvm_ota_confirm_running_image(void);

/* ---- pure helpers (host-testable, no ESP-IDF dependencies) ---- */

/** Parse a release-metadata JSON body. Returns false on malformed input. */
bool jetkvm_ota_parse_metadata(const char *json, size_t length,
                               jetkvm_ota_metadata_t *metadata);

/** A component is offered when its metadata version is non-empty and
 * semver-greater than the running version, like Go's
 * remoteVersion.GreaterThan(localVersion).  A non-empty constraint marks
 * a pinned custom-version install, where Go switches to plain inequality
 * so downgrades stay possible.  Unparseable versions produce no offer. */
bool jetkvm_ota_update_available(const char *local_version,
                                 const char *remote_version,
                                 const char *constraint);

/** Render the getUpdateStatus result object. */
bool jetkvm_ota_render_status_json(const jetkvm_ota_update_status_t *status,
                                   char *output, size_t capacity,
                                   size_t *out_size);

/** Render one {"jsonrpc":"2.0","method":"otaState",...} notification. */
bool jetkvm_ota_render_state_event(const jetkvm_ota_state_t *state,
                                   const char *timestamp,
                                   char *output, size_t capacity,
                                   size_t *out_size);

/**
 * True when the descriptor read from a downloaded image carries the magic
 * and names exactly this board's SKU.  Images from before the marker
 * (no magic) are rejected too: the cloud never serves them for this SKU.
 */
bool jetkvm_ota_image_desc_matches(const jetkvm_ota_image_desc_t *desc,
                                   const char *sku);

/** Validate an update API base URL (http/https, host present, no query). */
bool jetkvm_ota_validate_api_url(const char *url);

/** Build the release-metadata request URL; false when it would truncate.
 * The SKU selects the image built for this board variant. */
bool jetkvm_ota_build_releases_url(char *output, size_t capacity,
                                   const char *api_url, const char *device_id,
                                   const char *sku, bool include_prerelease,
                                   const char *app_constraint,
                                   const char *system_constraint);

/** Fill the JSON-RPC control-protocol context with this service's handlers. */
void jetkvm_ota_fill_control_context(jetkvm_ota_context_t *context);

#ifdef __cplusplus
}
#endif
