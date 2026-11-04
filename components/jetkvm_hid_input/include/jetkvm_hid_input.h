/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_control.h"
#include "jetkvm_usb_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Upstream applies HID RPC synchronously on its data-channel goroutine, so a
 * burst queues in SCTP and no report is dropped. The worker drains one busy
 * retry per tick (10 ms) while the host polls, so a 200 report/s scan needs
 * this much headroom to reach the host in full. */
#define JETKVM_HID_INPUT_QUEUE_LENGTH 128U
#define JETKVM_HID_INPUT_RESERVED_STATE_SLOTS 4U
#define JETKVM_HID_INPUT_KEYPRESS_RELEASE_MS 100U

/* Match the Go HIDRPC keypress keepalive policy. All timestamps are monotonic
 * microseconds captured at the submission boundary, never worker run time. */
#define JETKVM_HID_KEEPALIVE_EXPECTED_INTERVAL_US 50000LL
/* Upstream tolerates 50 ms of lateness. The Mini's inbound SCTP frames share
 * the peer loop with video encoding and RTP sends, so two consecutive
 * keepalives sent 50 ms apart can reach the worker up to ~150 ms apart while
 * the browser is holding a key. A late tick still shrinks the extension by
 * its lateness, so the hold cannot outlive the client's intent by more than
 * this bound; the base extension and staleness guard are unchanged. */
#define JETKVM_HID_KEEPALIVE_MAX_LATENESS_US 150000LL
/* Keeps upstream's relation: base extension = expected interval + max lateness. */
#define JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US 200000LL
#define JETKVM_HID_KEEPALIVE_MAX_STALENESS_US 225000LL

typedef enum {
    JETKVM_HID_KEEPALIVE_ACCEPTED = 0,
    JETKVM_HID_KEEPALIVE_JITTER_ADJUSTED,
    JETKVM_HID_KEEPALIVE_REJECTED_LATE,
    JETKVM_HID_KEEPALIVE_REJECTED_STALE,
    JETKVM_HID_KEEPALIVE_INVALID_ARGUMENT,
} jetkvm_hid_keepalive_result_t;

typedef struct {
    bool initialized;
    uint32_t generation;
    uint32_t epoch;
    int64_t last_arrival_us;
    int64_t last_timer_reset_us;
} jetkvm_hid_keepalive_policy_state_t;

typedef struct {
    jetkvm_hid_keepalive_result_t result;
    int64_t extension_us;
    int64_t deadline_us;
} jetkvm_hid_keepalive_decision_t;

typedef enum {
    JETKVM_HID_INPUT_SUBMIT_ACCEPTED = 0,
    JETKVM_HID_INPUT_SUBMIT_STALE_GENERATION,
    JETKVM_HID_INPUT_SUBMIT_QUEUE_FULL,
    JETKVM_HID_INPUT_SUBMIT_BACKPRESSURE,
    JETKVM_HID_INPUT_SUBMIT_NOT_READY,
    JETKVM_HID_INPUT_SUBMIT_NOT_SUPPORTED,
    JETKVM_HID_INPUT_SUBMIT_INVALID_ARGUMENT,
} jetkvm_hid_input_submit_result_t;

typedef struct {
    bool initialized;
    bool usb_available;
    bool accepting_input;
    uint32_t active_generation;
    uint8_t keyboard_leds;
    bool macro_active;
    bool macro_is_paste;
    uint32_t macro_step_index;
    uint32_t macro_step_count;
    uint64_t macro_state_revision;
    /* Advances with each keyboard report the host accepted. */
    uint32_t keyboard_revision;
    int64_t last_user_input_us;
    jetkvm_usb_hid_state_t usb;
    jetkvm_usb_hid_keyboard_state_t keyboard;
} jetkvm_hid_input_snapshot_t;

typedef struct {
    uint64_t submitted;
    uint64_t enqueued;
    uint64_t processed;
    uint64_t stale_generation_dropped;
    uint64_t queue_full_dropped;
    uint64_t absolute_move_dropped;
    uint64_t absolute_move_backpressure_dropped;
    uint64_t invalid_events;
    uint64_t macro_accepted;
    uint64_t macro_rejected;
    uint64_t macro_cancel_requests;
    uint64_t macro_cancelled;
    uint64_t macro_replaced;
    uint64_t macro_completed;
    uint64_t macro_failed;
    uint64_t usb_errors;
    uint64_t usb_report_retries;
    /* Keyboard report latency from transport accept to USB write done. */
    uint32_t keyboard_write_last_us;
    uint32_t keyboard_write_max_us;
    /* Keyboard reports that took more than 20 ms from accept to USB write. */
    uint32_t keyboard_write_slow;
    uint64_t keyboard_reports;
    uint64_t keypress_reports;
    uint64_t absolute_mouse_reports;
    uint64_t relative_mouse_reports;
    uint64_t local_relative_mouse_reports;
    uint64_t wheel_reports;
    uint64_t keepalives;
    uint64_t keepalives_rejected_late;
    uint64_t keepalives_rejected_stale;
    uint64_t keepalives_jitter_adjusted;
    uint64_t automatic_key_releases;
    uint64_t release_requests;
    uint64_t releases_completed;
    uint64_t release_retries;
    uint64_t release_failures;
    uint64_t release_blocked_dropped;
    uint64_t generation_changes;
    uint64_t led_updates;
    uint32_t queue_high_watermark;
    bool release_pending;
} jetkvm_hid_input_stats_t;

/** Reset all arrival history. The next valid tick becomes a fresh baseline. */
void jetkvm_hid_keepalive_policy_reset(
    jetkvm_hid_keepalive_policy_state_t *state);

/**
 * Evaluate one keepalive using its submission timestamp.
 *
 * Accepted decisions update `state` and return an absolute release deadline.
 * Rejected decisions leave `state` unchanged. A generation or epoch change
 * resets the history before evaluating the tick, preventing an old peer from
 * extending a new peer's key state. `evaluated_at_us` is used only to reject a
 * packet which itself spent longer than the staleness window in the worker
 * queue; it is never used as the base of an accepted deadline.
 */
jetkvm_hid_keepalive_decision_t jetkvm_hid_keepalive_policy_evaluate(
    jetkvm_hid_keepalive_policy_state_t *state,
    uint32_t generation,
    uint32_t epoch,
    int64_t received_at_us,
    int64_t evaluated_at_us);

/*
 * Start the CPU0 bridge worker. jetkvm_usb_hid_init() must have succeeded
 * before this call. This component does not own or uninstall the USB driver.
 */
esp_err_t jetkvm_hid_input_init(void);

/* Stop accepting input, release all HID state, and stop the bridge worker. */
esp_err_t jetkvm_hid_input_deinit(void);

/**
 * Pause or resume report acceptance while preserving the active WebRTC
 * generation and the bridge task.  Disable must run before target TinyUSB
 * teardown; enable must run after the target driver is installed again.
 */
esp_err_t jetkvm_hid_input_set_usb_available(bool available);

/*
 * Select the only WebRTC/session generation allowed to submit HID input.
 * Changing generation flushes the queue and places release-all at its head.
 * Generation zero is reserved for "no active peer".
 */
esp_err_t jetkvm_hid_input_set_active_generation(uint32_t generation);

/*
 * Invalidate generation if it is still active, flush pending input and enqueue
 * a high-priority release-all. A stale invalidate is an idempotent no-op.
 */
esp_err_t jetkvm_hid_input_invalidate_generation(uint32_t generation);

/* Flush pending input and asynchronously execute release-all on the worker. */
esp_err_t jetkvm_hid_input_release_all(void);

/*
 * Copy and enqueue one validated HIDRPC event without blocking. This function
 * always uses a zero-tick queue operation and never calls the USB driver.
 * Keyboard macros are copied into a fixed-size, single-entry latest mailbox.
 * Macro delays are handled by the worker's timer state machine and therefore
 * never block a regular key-up, cancellation, or generation teardown.
 */
jetkvm_hid_input_submit_result_t jetkvm_hid_input_submit_event(
    uint32_t generation, const jetkvm_hidrpc_event_t *event);

/* Unreliable movement preserves buttons from accepted reliable reports. */
jetkvm_hid_input_submit_result_t jetkvm_hid_input_submit_movement(
    uint32_t generation, const jetkvm_hidrpc_event_t *event);

/**
 * Queue a device-owned relative mouse move without requiring a live WebRTC
 * generation. This is reserved for persisted local automation such as the
 * mouse jiggler and still traverses the single USB worker/release barrier.
 */
esp_err_t jetkvm_hid_input_submit_local_relative_mouse(int8_t dx, int8_t dy);

/** Monotonic timestamp of the last successfully delivered user/HID report. */
int64_t jetkvm_hid_input_last_user_input_us(void);

/* Upstream attemptHidWriteRecovery: after three consecutive keyboard report
 * timeouts while the gadget is configured, the owner reconnects the gadget,
 * at most once every 30 s.  The handler runs on the HID worker and must hand
 * the reconnect to another task; the worker is what the reconnect waits for. */
typedef void (*jetkvm_hid_write_recovery_fn)(unsigned consecutive_timeouts,
                                             void *ctx);
esp_err_t jetkvm_hid_input_set_write_recovery_handler(
    jetkvm_hid_write_recovery_fn handler, void *ctx);

/* Called on the HID worker right after the host accepted a keyboard report
 * that changed the keys-down state, including automatic releases. The handler
 * must not block; it exists so a consumer can push the new state at once
 * instead of discovering it on its next poll. */
typedef void (*jetkvm_hid_keyboard_changed_fn)(void *ctx);
esp_err_t jetkvm_hid_input_set_keyboard_changed_handler(
    jetkvm_hid_keyboard_changed_fn handler, void *ctx);

esp_err_t jetkvm_hid_input_get_snapshot(
    jetkvm_hid_input_snapshot_t *snapshot);
esp_err_t jetkvm_hid_input_get_stats(jetkvm_hid_input_stats_t *stats);

/* Adapters for jetkvm_control_context_t state callbacks. */
bool jetkvm_hid_input_get_keyboard_led_state(
    jetkvm_keyboard_led_state_t *state, void *ctx);
bool jetkvm_hid_input_get_key_down_state(
    jetkvm_hidrpc_keyboard_report_t *state, void *ctx);
bool jetkvm_hid_input_get_usb_state(jetkvm_usb_state_t *state, void *ctx);
bool jetkvm_hid_input_get_usb_configuration(
    jetkvm_usb_configuration_snapshot_t *snapshot, void *ctx);
jetkvm_usb_wake_result_t jetkvm_hid_input_wake_host(void *ctx);

#ifdef __cplusplus
}
#endif
