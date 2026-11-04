/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * IP-KVM OS Services (protocol version 5): a sender on the target talks to
 * the KVM over a USB vendor interface. This component owns
 * that interface's session, its settings (the browser's OS Services
 * settings) and the KVM's identity key.
 *
 * The functions that take a ctx argument match the control protocol's
 * callbacks, so they can be installed there directly; ctx is unused.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Loads the settings and the identity key (making one on first boot), and
 * starts the service. Call from a task with an internal-RAM stack, before
 * TinyUSB is installed. On failure the USB interface still enumerates and
 * reports Faulted to the sender.
 */
esp_err_t jetkvm_os_services_init(void);

/* The service runs (perhaps Faulted); the browser may use it. */
bool jetkvm_os_services_is_started(void);

/* The next TinyUSB restart recovers from a fault. Unlike one for a mount or
 * a USB setting, it counts against the service video retry budget. Call
 * jetkvm_os_services_usb_recovery_done() when the restart call returns, so
 * that a restart that did not happen leaves the next one the KVM's. */
void jetkvm_os_services_usb_recovery_next(void);
void jetkvm_os_services_usb_recovery_done(void);

typedef struct {
    /* What getVideoState reports for OS Services changed. From the
     * service's task; must not block. */
    void (*state_changed)(void *ctx);
    /* The OS Services settings changed: send the osServicesSettings
     * event. From the task that changed them. */
    void (*settings_changed)(void *ctx);
    /* The browser's terminal and file channels have a message to send or a
     * channel to close (jetkvm_os_services_channel_next_output()). From any
     * task; must not block. */
    void (*channel_output)(void *ctx);
    void *ctx;
} jetkvm_os_services_listener_t;

void jetkvm_os_services_set_listener(
    const jetkvm_os_services_listener_t *listener);

/* Adds the OS Services settings' fields to object. */
bool jetkvm_os_services_get_settings_json(cJSON *object, void *ctx);

/*
 * Lays the fields present in request over the current settings, saves and
 * applies them, and adds the applied settings to applied. Returns NULL, or
 * why the request was refused (the settings are then unchanged). It writes
 * flash, so the caller's stack must be in internal RAM.
 */
const char *jetkvm_os_services_set_settings_json(const cJSON *request,
                                                 cJSON *applied, void *ctx);

/* Adds source, osServices and the guest fields of getVideoState and
 * videoInputState to object. */
bool jetkvm_os_services_add_video_state(cJSON *object, void *ctx);

/* The browser's guestUpdateStart: installs the newest OS Services release on
 * the connected sender. Returns NULL, or why it cannot start. */
const char *jetkvm_os_services_update_start(void *ctx);

/* {"jsonrpc":"2.0","method":"osServicesSettings","params":{"sequence":N,
 * "settings":{...}}} */
bool jetkvm_os_services_render_settings_event(char *output, size_t capacity,
                                              size_t *length);

/*
 * Reads the target's clipboard through the sender, waiting for its answer.
 * Returns NULL with *status set ("ok", "unsupported", "empty", "denied" or
 * "failed") and, for "ok", the text in text[0..*length); or the reason no
 * answer came. At most capacity bytes of text are kept; *truncated says
 * whether the sender or this call cut it.
 */
const char *jetkvm_os_services_clipboard_get(char *text, size_t capacity,
                                             size_t *length, bool *truncated,
                                             const char **status, void *ctx);

/* Writes text (UTF-8, at most 65528 bytes) to the target's clipboard,
 * waiting for the sender's answer, as clipboard_get. */
const char *jetkvm_os_services_clipboard_set(const char *text, size_t length,
                                             const char **status, void *ctx);

/*
 * guestFileDrop: offers files, an array of {"name": string, "size":
 * integer} the caller has checked, to the sender in one file.drop, waiting
 * until it is written (about 2.5 seconds at most). Adds "channel" (the data
 * channel the browser then opens) and "transfers" (one ID per file) to
 * result. Returns NULL, or why no drop started.
 */
const char *jetkvm_os_services_file_drop(const cJSON *files, cJSON *result,
                                         void *ctx);

/*
 * The browser's "guest-terminal" and "guestfile_<drop>" data channels, for
 * the WebRTC transport, which calls these from its task. channel is the
 * transport's ID for a channel: never 0, and not reused while the channel is
 * known. None of them waits for the sender: the service's task relays the
 * messages, and these only copy under a lock.
 */

/* A message to send on a channel, or the channel to close. */
typedef struct {
    uint32_t channel;
    /* Close the channel; everything before it was sent. */
    bool close;
    bool text;
    /* Valid until jetkvm_os_services_channel_output_done(). */
    const uint8_t *data;
    size_t length;
} jetkvm_os_services_channel_output_t;

/* A channel opened. Returns true when it is a terminal or file channel and
 * the service took it (it has memory for its buffers); the transport closes
 * it otherwise. */
bool jetkvm_os_services_channel_claim(uint32_t channel, const char *label);
/* A message of a claimed channel; data is only valid for the call. */
void jetkvm_os_services_channel_message(uint32_t channel, bool text,
                                        const uint8_t *data, size_t length);
/* A claimed channel closed, from either side, or the peer went away. The
 * last call for the channel. */
void jetkvm_os_services_channel_closed(uint32_t channel);
/* The next message to send or channel to close; false when there is none.
 * jetkvm_os_services_channel_output_done() follows before the next. */
bool jetkvm_os_services_channel_next_output(
    jetkvm_os_services_channel_output_t *output);
/* sent is false when the transport could not send it now; the same output
 * comes again. */
void jetkvm_os_services_channel_output_done(uint32_t channel, bool sent);

#ifdef __cplusplus
}
#endif
