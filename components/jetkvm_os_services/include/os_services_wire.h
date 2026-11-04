/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * IP-KVM OS Services protocol, KVM side: constants, the message envelope
 * and little-endian payload helpers. Section names in comments refer to
 * sections of the OS Services protocol, version 5.0. Nothing here depends
 * on ESP-IDF, so the host tests compile it as is.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OSVC_PROTOCOL_MAJOR 5U
#define OSVC_PROTOCOL_MINOR 0U

/* USB interface (Transport, USB interface). */
#define OSVC_USB_CLASS 0xffU
#define OSVC_USB_SUBCLASS 0x4aU
#define OSVC_USB_PROTOCOL 0x01U
#define OSVC_USB_REQUEST_RESET 0x40U
#define OSVC_USB_REQUEST_STATUS 0x41U
#define OSVC_USB_STATUS_LEN 24U
#define OSVC_TOKEN_LEN 16U

typedef enum {
    OSVC_PHASE_RESETTING = 0,
    OSVC_PHASE_READY = 1,
    OSVC_PHASE_ACTIVE = 2,
    OSVC_PHASE_FAULTED = 3,
} osvc_phase_t;

/* Envelope. */
#define OSVC_ENVELOPE_LEN 8U
#define OSVC_MAX_CONTROL_PAYLOAD (64U * 1024U)
#define OSVC_MAX_FRAME_PAYLOAD (8U * 1024U * 1024U)
#define OSVC_MAX_DIMENSION 16384U
#define OSVC_MAX_FPS 240U
/* The KVM searches this much input for the session.hello of a new session. */
#define OSVC_HELLO_SEARCH_LIMIT (8U * 1024U * 1024U)

typedef enum {
    OSVC_SERVICE_SESSION = 0,
    OSVC_SERVICE_VIDEO = 1,
    OSVC_SERVICE_CLIPBOARD = 2,
    OSVC_SERVICE_TERMINAL = 3,
    OSVC_SERVICE_FILE = 4,
    OSVC_SERVICE_UPDATE = 5,
    OSVC_SERVICE_COUNT = 6,
} osvc_service_t;

enum {
    OSVC_SESSION_HELLO = 1,
    OSVC_SESSION_READY = 2,
    OSVC_SESSION_PING = 3,
    OSVC_SESSION_CLOSE = 4,
    OSVC_SESSION_CONFIG = 5,
    OSVC_SESSION_INFO = 6,
};

enum {
    OSVC_CLOSE_SHUTDOWN = 0,
    OSVC_CLOSE_RESERVED = 1,
    OSVC_CLOSE_PROTOCOL_ERROR = 2,
    OSVC_CLOSE_NOT_APPROVED = 3,
};

#define OSVC_HELLO_LEN 52U
#define OSVC_READY_LEN 116U
#define OSVC_READY_SIGNED_LEN 52U
#define OSVC_PUBLIC_KEY_LEN 32U
#define OSVC_SIGNATURE_LEN 64U
#define OSVC_NONCE_LEN 32U
#define OSVC_SIGNATURE_CONTEXT "IP-KVM OS Services session.ready v5"
#define OSVC_FINGERPRINT_LEN 8U
/* "AA:BB:CC:DD:EE:FF:00:11" plus the terminator. */
#define OSVC_FINGERPRINT_TEXT_CAPACITY 24U

/* session.info limits. */
#define OSVC_INFO_VERSION_MAX 64U
#define OSVC_INFO_DISPLAY_MAX 256U
#define OSVC_INFO_DISPLAYS_MAX 64U

enum {
    OSVC_VIDEO_CAPABILITIES = 1,
    OSVC_VIDEO_CONFIG = 3,
    OSVC_VIDEO_STATUS = 4,
    OSVC_VIDEO_FRAME = 5,
    OSVC_VIDEO_REQUEST_KEYFRAME = 6,
};

#define OSVC_VIDEO_CODEC_FLAG_H264 (1U << 0)
#define OSVC_VIDEO_CODEC_FLAG_H265 (1U << 1)
/* The sender sets the mode of the KVM display. A sender sets it only for a
 * KVM whose session.config has "display_mode_by_sender". */
#define OSVC_VIDEO_FLAG_SETS_DISPLAY_MODE (1U << 2)
#define OSVC_VIDEO_CAPABILITIES_LEN 16U
#define OSVC_VIDEO_CONFIG_LEN 20U
#define OSVC_VIDEO_STATUS_MIN_LEN 32U
#define OSVC_VIDEO_FRAME_HEADER_LEN 12U
#define OSVC_VIDEO_FRAME_KEYFRAME (1U << 0)
#define OSVC_VIDEO_FRAME_PARAMETER_SETS (1U << 1)
#define OSVC_VIDEO_ERROR_DISPLAY_INACTIVE 1U

typedef enum {
    OSVC_CODEC_H264 = 0,
    OSVC_CODEC_H265 = 1,
} osvc_codec_t;

typedef enum {
    OSVC_LAYOUT_MIRROR = 0,
    OSVC_LAYOUT_EXTEND = 1,
} osvc_layout_t;

enum {
    OSVC_CLIPBOARD_CAPABILITIES = 1,
    OSVC_CLIPBOARD_GET = 2,
    OSVC_CLIPBOARD_CONTENT = 3,
    OSVC_CLIPBOARD_SET = 4,
    OSVC_CLIPBOARD_STATUS = 5,
    OSVC_CLIPBOARD_CONFIG = 7,
};

#define OSVC_CLIPBOARD_FLAG_READ (1U << 0)
#define OSVC_CLIPBOARD_FLAG_WRITE (1U << 1)
#define OSVC_CLIPBOARD_MAX_TEXT 65528U

typedef enum {
    OSVC_CLIPBOARD_OK = 0,
    OSVC_CLIPBOARD_UNSUPPORTED = 1,
    OSVC_CLIPBOARD_EMPTY = 2,
    OSVC_CLIPBOARD_DENIED = 3,
    OSVC_CLIPBOARD_FAILED = 4,
} osvc_clipboard_status_t;

typedef enum {
    OSVC_OS_UNKNOWN = 0,
    OSVC_OS_WINDOWS = 1,
    OSVC_OS_MACOS = 2,
    OSVC_OS_LINUX = 3,
} osvc_target_os_t;

enum {
    OSVC_TERMINAL_CAPABILITIES = 1,
    OSVC_TERMINAL_OPEN = 2,
    OSVC_TERMINAL_OPENED = 3,
    OSVC_TERMINAL_INPUT = 4,
    OSVC_TERMINAL_OUTPUT = 5,
    OSVC_TERMINAL_RESIZE = 6,
    OSVC_TERMINAL_CLOSE = 7,
    OSVC_TERMINAL_EXITED = 8,
    OSVC_TERMINAL_CREDIT = 9,
};

#define OSVC_TERMINAL_FLAG_SHELL (1U << 0)
/* session_id (4) precedes the data of terminal.input and terminal.output. */
#define OSVC_TERMINAL_MAX_DATA (OSVC_MAX_CONTROL_PAYLOAD - 4U)
/* terminal.open and terminal.resize: session_id u32, rows u16, cols u16. */
#define OSVC_TERMINAL_SIZE_LEN 8U
/* terminal.opened and terminal.exited: session_id u32 and one byte. */
#define OSVC_TERMINAL_STATUS_LEN 5U
/* terminal.credit: session_id u32, bytes u32. */
#define OSVC_TERMINAL_CREDIT_LEN 8U
/* terminal.output data a terminal session may have beyond the credit the
 * KVM returned: the KVM's buffer for it (OSVC_TERMINAL_OUTPUT_BYTES). */
#define OSVC_TERMINAL_OUTPUT_WINDOW (128U * 1024U)

enum {
    OSVC_TERMINAL_OPENED_OK = 0,
    OSVC_TERMINAL_OPENED_UNSUPPORTED = 1,
    OSVC_TERMINAL_OPENED_BUSY = 2,
    OSVC_TERMINAL_OPENED_FAILED = 3,
};

enum {
    OSVC_TERMINAL_EXIT_EXITED = 0,
    OSVC_TERMINAL_EXIT_CLOSED = 1,
    OSVC_TERMINAL_EXIT_FAILED = 2,
};

enum {
    OSVC_FILE_CAPABILITIES = 1,
    OSVC_FILE_DROP = 2,
    OSVC_FILE_READY = 3,
    OSVC_FILE_CHUNK = 4,
    OSVC_FILE_CREDIT = 5,
    OSVC_FILE_DONE = 6,
    OSVC_FILE_CANCEL = 7,
};

#define OSVC_FILE_FLAG_RECEIVE (1U << 0)
/* transfer_id (4) and offset (8) precede the data of a chunk. */
#define OSVC_CHUNK_HEADER_LEN 12U
#define OSVC_CHUNK_MAX_DATA (OSVC_MAX_CONTROL_PAYLOAD - OSVC_CHUNK_HEADER_LEN)
/* file.drop: file_count u32, then per file transfer_id u32, size u64,
 * name_length u16 and the name. */
#define OSVC_FILE_DROP_HEADER_LEN 4U
#define OSVC_FILE_DROP_ENTRY_LEN 14U
/* file.ready and file.credit: transfer_id u32 and a byte count u32. */
#define OSVC_FILE_CREDIT_LEN 8U
/* file.done: transfer_id u32, status u8, three reserved bytes, path. */
#define OSVC_FILE_DONE_FIXED_LEN 8U

/* file.done status. */
typedef enum {
    OSVC_FILE_DONE_OK = 0,
    OSVC_FILE_DONE_FAILED = 1,
    OSVC_FILE_DONE_CANCELLED = 2,
} osvc_file_status_t;

enum {
    OSVC_UPDATE_CAPABILITIES = 1,
    OSVC_UPDATE_BEGIN = 2,
    OSVC_UPDATE_READY = 3,
    OSVC_UPDATE_CHUNK = 4,
    OSVC_UPDATE_CREDIT = 5,
    OSVC_UPDATE_STATUS = 6,
    OSVC_UPDATE_CANCEL = 7,
};

#define OSVC_UPDATE_FLAG_INSTALL (1U << 0)
#define OSVC_UPDATE_PACKAGE_MAX 64U
#define OSVC_UPDATE_VERSION_MAX 64U
#define OSVC_UPDATE_BEGIN_FIXED_LEN 44U

enum {
    OSVC_UPDATE_STATUS_INSTALLING = 0,
    OSVC_UPDATE_STATUS_FAILED = 1,
    OSVC_UPDATE_STATUS_CANCELLED = 2,
};

static inline uint16_t osvc_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t osvc_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t osvc_get_u64(const uint8_t *p)
{
    return (uint64_t)osvc_get_u32(p) | ((uint64_t)osvc_get_u32(p + 4) << 32);
}

static inline void osvc_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void osvc_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void osvc_put_u64(uint8_t *p, uint64_t v)
{
    osvc_put_u32(p, (uint32_t)v);
    osvc_put_u32(p + 4, (uint32_t)(v >> 32));
}

/* Writes the 8-byte envelope for a message. */
static inline void osvc_put_envelope(uint8_t *p, uint16_t service,
                                     uint16_t kind, uint32_t payload_size)
{
    osvc_put_u16(p, service);
    osvc_put_u16(p + 2, kind);
    osvc_put_u32(p + 4, payload_size);
}

/* The payload limit of a message: 8 MiB for video.frame, else 64 KiB. */
static inline uint32_t osvc_payload_limit(uint16_t service, uint16_t kind)
{
    return service == OSVC_SERVICE_VIDEO && kind == OSVC_VIDEO_FRAME ?
               OSVC_MAX_FRAME_PAYLOAD : OSVC_MAX_CONTROL_PAYLOAD;
}

/* Strict UTF-8 check: no overlongs, surrogates or code points past U+10FFFF. */
bool osvc_utf8_valid(const uint8_t *text, size_t length);

/*
 * A file.drop name ("File service"): one path component of valid UTF-8,
 * not empty, not "." or "..", without '/', '\\' or a control character
 * (U+0000 to U+001F and U+007F to U+009F).
 */
bool osvc_file_name_valid(const uint8_t *name, size_t length);

/* Formats the first 8 bytes of SHA-256(public key) as "AA:BB:...". */
void osvc_format_fingerprint(const uint8_t digest[OSVC_FINGERPRINT_LEN],
                             char out[OSVC_FINGERPRINT_TEXT_CAPACITY]);

/*
 * Annex B helpers for video.frame checks. They look at the first VCL NAL
 * unit (the rule in "video.frame") and at whether the access unit carries
 * the parameter sets of its codec.
 */
bool osvc_annexb_is_keyframe(osvc_codec_t codec, const uint8_t *data,
                             size_t length);
bool osvc_annexb_has_parameter_sets(osvc_codec_t codec, const uint8_t *data,
                                    size_t length);

#ifdef __cplusplus
}
#endif
