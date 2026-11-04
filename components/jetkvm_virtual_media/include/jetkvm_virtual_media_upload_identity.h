/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

/*
 * Reserved resume-identity value for Go-compatible upload clients that provide
 * only filename and size.  This is not a digest of the payload and must never
 * be presented as an end-to-end integrity result.  When it is requested, a
 * valid persisted sidecar with the same filename and size may be resumed
 * regardless of its stored SHA-256; callers that provide a real digest retain
 * strict size-and-digest matching.
 */
#define JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE                  \
    "00000000000000000000000000000000"                                 \
    "00000000000000000000000000000000"

