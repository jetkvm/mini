/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_virtual_media.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One HTTP(S) GET streamed into a file in the TF card's images folder.
 *
 * The body goes through the resumable upload session that browser uploads
 * use: it is written to <filename>.incomplete and renamed to <filename> only
 * after the last byte, so a partial file never carries the final name. A
 * failed download keeps the .incomplete prefix; the next download of the
 * same object continues from it with a Range request. A complete file of the
 * same size is reused without a transfer; one of another size is replaced.
 */

#define JETKVM_STORAGE_DOWNLOAD_ERROR_CAPACITY 128U

typedef enum {
    JETKVM_STORAGE_DOWNLOAD_IDLE = 0,
    JETKVM_STORAGE_DOWNLOAD_DOWNLOADING,
    JETKVM_STORAGE_DOWNLOAD_DONE,
    JETKVM_STORAGE_DOWNLOAD_ERROR,
} jetkvm_storage_download_phase_t;

typedef struct {
    jetkvm_storage_download_phase_t phase;
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    /** Bytes in the file on the card, a resumed prefix included. */
    uint64_t bytes;
    /** Size of the object; 0 until the server reported it. */
    uint64_t total;
    /** DONE without a transfer: the card already had the file. */
    bool reused;
    /** The transfer holds video off (video_pause hook). */
    bool video_paused;
    char error[JETKVM_STORAGE_DOWNLOAD_ERROR_CAPACITY];
} jetkvm_storage_download_state_t;

/** Invoked from the download task after each state change worth pushing. */
typedef void (*jetkvm_storage_download_listener_fn)(void *ctx);

/**
 * Pause (true) or resume (false) video delivery. Called from the download
 * task before the body transfer starts and once after it ends, whatever the
 * outcome. Sending no frames frees the Wi-Fi link for the download.
 */
typedef void (*jetkvm_storage_download_video_pause_fn)(bool paused, void *ctx);

typedef struct {
    jetkvm_storage_download_listener_fn listener;
    void *listener_ctx;
    jetkvm_storage_download_video_pause_fn video_pause;
    void *video_pause_ctx;
} jetkvm_storage_download_hooks_t;

/** Install the hooks before the first download starts. */
void jetkvm_storage_download_set_hooks(
    const jetkvm_storage_download_hooks_t *hooks);

/**
 * Start a download in the background. Refuses at once, with a reason in
 * `error`, when a download already runs (ESP_ERR_INVALID_STATE), an upload
 * holds the storage (ESP_ERR_INVALID_STATE), there is no mounted TF card
 * (ESP_ERR_NOT_FOUND) or the URL or filename is invalid
 * (ESP_ERR_INVALID_ARG). Everything later is reported through the state.
 */
esp_err_t jetkvm_storage_download_start(const char *url, const char *filename,
                                        char *error, size_t error_capacity);

/** Copy the state of the current or most recent download. */
void jetkvm_storage_download_get_state(jetkvm_storage_download_state_t *state);

#ifdef __cplusplus
}
#endif
