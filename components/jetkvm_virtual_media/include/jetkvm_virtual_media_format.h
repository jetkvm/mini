/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

/** The outcome of a request to format the TF card. */
typedef enum {
    JETKVM_VIRTUAL_MEDIA_FORMAT_COMPLETED = 0,
    /** The storage service has not finished its start. */
    JETKVM_VIRTUAL_MEDIA_FORMAT_NOT_READY,
    /** The socket has no card. */
    JETKVM_VIRTUAL_MEDIA_FORMAT_NO_CARD,
    /** An image is mounted, from the card or from an HTTP(S) server. */
    JETKVM_VIRTUAL_MEDIA_FORMAT_IMAGE_MOUNTED,
    /** An upload or a download writes to the card. */
    JETKVM_VIRTUAL_MEDIA_FORMAT_UPLOAD_ACTIVE,
    /** The card did not take the new filesystem. */
    JETKVM_VIRTUAL_MEDIA_FORMAT_FAILED,
} jetkvm_virtual_media_format_result_t;
