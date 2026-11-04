/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_usb_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool desired_enabled;
    bool driver_enabled;
    bool hid_input_ready;
    bool transitioning;
    bool command_pending;
    bool configuration_ready;
    bool configuration_pending;
    bool configuration_transitioning;
    bool configuration_store_degraded;
    uint32_t lifecycle_generation;
    uint32_t commands_accepted;
    uint32_t commands_replaced;
    uint32_t transitions_completed;
    uint32_t transition_failures;
    uint32_t configuration_generation;
    uint32_t configuration_requests;
    uint32_t configuration_transactions_completed;
    uint32_t configuration_transaction_failures;
    uint32_t configuration_rollbacks;
    uint32_t configuration_rollback_failures;
    uint32_t configuration_wait_timeouts;
    esp_err_t last_error;
    esp_err_t configuration_load_error;
    esp_err_t last_configuration_error;
} jetkvm_usb_service_snapshot_t;

typedef void (*jetkvm_usb_service_state_callback_t)(
    const jetkvm_usb_service_snapshot_t *snapshot, void *ctx);

typedef enum {
    JETKVM_USB_SERVICE_REQUEST_ACCEPTED = 0,
    JETKVM_USB_SERVICE_REQUEST_UNAVAILABLE,
} jetkvm_usb_service_request_result_t;

typedef enum {
    JETKVM_USB_SERVICE_CONFIGURATION_COMPLETED = 0,
    JETKVM_USB_SERVICE_CONFIGURATION_INVALID,
    JETKVM_USB_SERVICE_CONFIGURATION_UNSUPPORTED,
    JETKVM_USB_SERVICE_CONFIGURATION_FAILED,
    JETKVM_USB_SERVICE_CONFIGURATION_UNAVAILABLE,
    JETKVM_USB_SERVICE_CONFIGURATION_BUSY,
} jetkvm_usb_service_configuration_result_t;

/**
 * Start the sole owner of the P4 target USB lifecycle.
 *
 * The owner is pinned to CPU0 and is the only task allowed to initialize or
 * uninstall the HS/UTMI TinyUSB driver and its HID-input bridge.  The call
 * waits only for the bounded initial enable attempt; target VBUS is optional.
 * The owner remains active if that attempt fails or times out, and the latest
 * outcome is available through the snapshot. The callback runs on the owner
 * task and must not block or call this service synchronously.
 */
esp_err_t jetkvm_usb_service_start(
    jetkvm_usb_service_state_callback_t callback, void *callback_ctx);

/**
 * Publish an idempotent latest-state enable/disable request without blocking.
 * A task notification wakes the owner; the command payload remains in one
 * atomic desired-state slot, so concurrent requests cannot execute out of
 * order and a later request may replace work which has not started yet.
 */
jetkvm_usb_service_request_result_t
jetkvm_usb_service_request_enabled(bool enabled);

/** Copy the last fully applied and persisted USB configuration. */
esp_err_t jetkvm_usb_service_get_configuration(
    jetkvm_usb_config_t *configuration);

/**
 * Apply a complete USB configuration through the sole owner task.
 *
 * The call is synchronous. The owner re-enumerates the target HS/UTMI port
 * when it is running, persists through the CRC-protected A/B store, and
 * publishes the candidate only after both steps succeed. After the six-second
 * operation deadline, the caller requests cancellation and remains blocked
 * until the owner confirms completion or rollback; FAILED therefore never
 * means that the same candidate can commit later. On any failure it restores
 * the previous descriptor/runtime configuration. A second simultaneous
 * configuration call returns BUSY; it never queues a FIFO.
 */
jetkvm_usb_service_configuration_result_t
jetkvm_usb_service_apply_configuration(
    const jetkvm_usb_config_t *configuration);

/**
 * Safely detach and reattach the current composite target device through the
 * USB owner task without changing or persisting its descriptor configuration.
 * This is used when the host has to learn the drive of a mounted image
 * again, see jetkvm_usb_service_present_medium().
 */
jetkvm_usb_service_configuration_result_t
jetkvm_usb_service_reenumerate(void);

/**
 * Make the host find the image that has just been mounted. A CD-ROM image is
 * a new medium of a drive that the host has as a CD-ROM drive and asks for
 * its medium: the drive reports the change, and the USB device stays as it
 * is. Every other mount re-enumerates as jetkvm_usb_service_reenumerate()
 * does: a change of the drive's type needs it on every host, and macOS stops
 * asking a drive that has had a medium, so it finds the next image only in a
 * drive that it has enumerated again.
 */
jetkvm_usb_service_configuration_result_t
jetkvm_usb_service_present_medium(bool cdrom_image);

/** Complete an enable/disable operation through the owner, or report failure.
 * Uses the same serialization, cancellation and rollback as descriptor changes.
 * Call from a worker, never from the peer callback or the USB owner itself.
 */
jetkvm_usb_service_configuration_result_t jetkvm_usb_service_set_enabled(bool enabled);

esp_err_t jetkvm_usb_service_get_snapshot(
    jetkvm_usb_service_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
