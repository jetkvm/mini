/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>

#include "jetkvm_virtual_media_format.h"

/** What the storage service has open at a request to format the card. */
typedef struct {
    bool initialized;
    /** The card-detect input reads a card at the time of the request. */
    bool card_present;
    /** An image is mounted, from the card or from an HTTP(S) server. The
     * format holds the media lock that every read of the host waits for. */
    bool image_mounted;
    /** An upload or a download holds the writer lease, or is taking it. */
    bool upload_has_writer;
    /** The file of an upload stays open after its close failed. */
    bool upload_close_failed;
} jetkvm_storage_format_inputs_t;

/**
 * JETKVM_VIRTUAL_MEDIA_FORMAT_COMPLETED when the card may be formatted now,
 * otherwise the first reason why not, in this order: not ready, no card, an
 * image mounted, an upload writing. An upload without a writer is closed by
 * the format.
 */
jetkvm_virtual_media_format_result_t jetkvm_storage_format_admit(
    const jetkvm_storage_format_inputs_t *inputs);
