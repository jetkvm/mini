/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The OS Services USB transport: a TinyUSB class driver for the interface
 * jetkvm_usb_hid describes, and the vendor RESET and STATUS requests.
 *
 * TinyUSB's task answers the control requests at once and runs the bulk
 * endpoints. The service task owns the session: it reads the events and the
 * OUT data here, writes the IN data, and publishes the status that STATUS
 * reports. Nothing here blocks TinyUSB's task.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "os_services_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* RESETs answered since boot, and the token of the latest. */
    uint32_t reset_sequence;
    uint8_t token[OSVC_TOKEN_LEN];
    /* Bus resets, configuration changes, TinyUSB restarts and failed bulk
     * transfers since boot. Each one ends the session. */
    uint32_t reconfigure_sequence;
    /* reconfigure_sequence right after the KVM's latest TinyUSB restart for
     * a mount or a USB setting (not for a fault recovery). */
    uint32_t restart_reconfigure_sequence;
    /* reconfigure_sequence when the latest RESET arrived. A RESET followed
     * by a reconfiguration admits no session. */
    uint32_t reset_reconfigure_sequence;
    /* The host configured the device and the interface's endpoints are
     * open. */
    bool configured;
    bool high_speed;
} osvc_usb_events_t;

typedef struct {
    uint64_t out_bytes;
    uint64_t in_bytes;
    uint32_t out_transfers;
    uint32_t in_transfers;
    uint32_t zero_length_packets;
    uint32_t resets;
    uint32_t statuses;
    uint32_t stalled_requests;
    uint32_t reconfigures;
    uint32_t dropped_reads;
    /* How long a filled OUT read waited for the service task once the task
     * had given back the one before: the longest, and how many waited more
     * than OSVC_USB_SLOW_READ_MS. The bus NAKs meanwhile. */
    uint32_t read_wait_max_us;
    uint32_t slow_reads;
} osvc_usb_counters_t;

#define OSVC_USB_SLOW_READ_MS 20

/*
 * Allocates the transfer buffers and registers the class driver's hooks
 * with jetkvm_usb_hid. Call once, before TinyUSB is installed. task is
 * notified (xTaskNotifyGive) whenever there is something to read. On
 * failure the interface still enumerates and reports Faulted.
 */
esp_err_t osvc_usb_init(TaskHandle_t task);

void osvc_usb_get_events(osvc_usb_events_t *events);

/* The status STATUS answers while no newer RESET has arrived than
 * reset_sequence. */
void osvc_usb_publish_status(uint32_t reset_sequence,
                             const uint8_t status[OSVC_USB_STATUS_LEN]);

/* Starts the transport for the session that followed reset_sequence: drops
 * the IN bytes and OUT reads of the previous one and reads OUT again. A
 * transfer already on the bus completes, and the sender discards it while it
 * searches for session.ready. */
void osvc_usb_restart(void);

/* The session ended: stops reading OUT and drops queued IN bytes, until the
 * next restart. */
void osvc_usb_stop(void);

/* The next OUT read, in order. Valid until osvc_usb_receive_done(). */
bool osvc_usb_receive(const uint8_t **data, size_t *length);
void osvc_usb_receive_done(void);

/*
 * Queues one message for the IN endpoint, its parts in order and all of
 * them or none, waiting up to timeout_ms for room. Gives up at once when a
 * RESET or a reconfiguration arrives.
 */
bool osvc_usb_write(const uint8_t *const parts[], const size_t lengths[],
                    size_t count, uint32_t timeout_ms);

/* How long the IN endpoint has had a transfer the host has not read, in
 * milliseconds, or 0. */
int64_t osvc_usb_in_waiting_ms(void);

void osvc_usb_get_counters(osvc_usb_counters_t *counters);

#ifdef __cplusplus
}
#endif
