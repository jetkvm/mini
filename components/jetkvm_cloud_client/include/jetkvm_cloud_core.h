#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jetkvm_cloud_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These limits are deliberately independent of ESP-IDF so the framing and
 * trust-boundary logic can be exercised by a normal host compiler. */
#define JETKVM_CLOUD_CORE_MAX_URL_SIZE          512U
#define JETKVM_CLOUD_CORE_MAX_DEVICE_ID_SIZE    128U
#define JETKVM_CLOUD_CORE_MAX_APP_VERSION_SIZE  128U
#define JETKVM_CLOUD_CORE_MAX_SKU_SIZE          64U
#define JETKVM_CLOUD_CORE_MAX_TOKEN_SIZE        1024U
/* Added to the firmware version in X-App-Version. The cloud dashboard opens a
 * device at app.jetkvm.com/v/<X-App-Version>/, and the Mini release uploads
 * its UI bundle there as <version>-mini, so a Mini never gets the JetKVM
 * bundle of the same version number. Only the cloud sees the suffix; OTA
 * compares the plain firmware version. */
/* Four header values at their maxima plus the fixed header names, CRLFs and
 * the terminator ("X-Device-ID: ", "X-App-Version: ", "X-Device-SKU: ",
 * "Authorization: Bearer ", four CRLFs and the 5-byte version suffix total
 * 85 bytes). */
#define JETKVM_CLOUD_CORE_MAX_HEADER_SIZE \
    (JETKVM_CLOUD_CORE_MAX_DEVICE_ID_SIZE + JETKVM_CLOUD_CORE_MAX_APP_VERSION_SIZE + \
     JETKVM_CLOUD_CORE_MAX_SKU_SIZE + JETKVM_CLOUD_CORE_MAX_TOKEN_SIZE + 96U)
#define JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE      65536U

/* RFC 6455 opcodes used by esp_websocket_client event metadata. */
#define JETKVM_CLOUD_WS_OPCODE_CONTINUATION 0x00U
#define JETKVM_CLOUD_WS_OPCODE_TEXT         0x01U
#define JETKVM_CLOUD_WS_OPCODE_BINARY       0x02U
#define JETKVM_CLOUD_WS_OPCODE_CLOSE        0x08U
#define JETKVM_CLOUD_WS_OPCODE_PING         0x09U
#define JETKVM_CLOUD_WS_OPCODE_PONG         0x0aU

typedef enum {
    JETKVM_CLOUD_CORE_OK = 0,
    JETKVM_CLOUD_CORE_INVALID_ARGUMENT,
    JETKVM_CLOUD_CORE_INVALID_URL,
    JETKVM_CLOUD_CORE_INVALID_HEADER_VALUE,
    JETKVM_CLOUD_CORE_OUTPUT_TOO_SMALL,
    JETKVM_CLOUD_CORE_PROTOCOL_ERROR,
    JETKVM_CLOUD_CORE_MESSAGE_TOO_LARGE,
} jetkvm_cloud_core_status_t;

/**
 * Convert only the URI scheme used by the configured JetKVM cloud base URL.
 * The authority, path and query are copied byte-for-byte; no endpoint path or
 * trailing slash is appended. Fragments and HTTP userinfo are rejected.
 */
jetkvm_cloud_core_status_t jetkvm_cloud_core_make_ws_url(
    const char *cloud_url, char *output, size_t output_capacity,
    bool *uses_tls);

/** Render the exact four JetKVM device upgrade headers, each ending in CRLF. */
jetkvm_cloud_core_status_t jetkvm_cloud_core_make_headers(
    const char *device_id, const char *app_version, const char *sku,
    const char *token, char *output, size_t output_capacity,
    size_t *output_length);

typedef enum {
    /** A data/control event was valid but did not finish a text message. */
    JETKVM_CLOUD_REASSEMBLY_INCOMPLETE = 0,
    /** One complete text message is available through message_data/size. */
    JETKVM_CLOUD_REASSEMBLY_COMPLETE,
    /** A control frame was ignored without disturbing an in-progress message. */
    JETKVM_CLOUD_REASSEMBLY_CONTROL,
    JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR,
    JETKVM_CLOUD_REASSEMBLY_TOO_LARGE,
} jetkvm_cloud_reassembly_result_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t message_length;
    size_t frame_base;
    size_t frame_length;
    size_t frame_received;
    uint8_t frame_opcode;
    bool frame_fin;
    bool message_active;
    bool frame_active;
    bool complete;
} jetkvm_cloud_reassembler_t;

/** capacity includes the byte reserved for a terminating NUL. */
void jetkvm_cloud_reassembler_init(jetkvm_cloud_reassembler_t *state,
                                   uint8_t *buffer, size_t capacity);
void jetkvm_cloud_reassembler_reset(jetkvm_cloud_reassembler_t *state);

/**
 * Consume one esp_websocket_client DATA event. payload_length and
 * payload_offset describe the current WebSocket frame, while fin is the frame
 * FIN bit. Continuation frames and delivery of one frame through multiple
 * events are both supported and independently validated.
 */
jetkvm_cloud_reassembly_result_t jetkvm_cloud_reassembler_push(
    jetkvm_cloud_reassembler_t *state, uint8_t opcode, bool fin,
    size_t payload_length, size_t payload_offset,
    const uint8_t *data, size_t data_length);

const uint8_t *jetkvm_cloud_reassembler_message_data(
    const jetkvm_cloud_reassembler_t *state);
size_t jetkvm_cloud_reassembler_message_size(
    const jetkvm_cloud_reassembler_t *state);

typedef struct {
    uint32_t connection_epoch;
    bool connected;
    bool session_active;
    uint32_t session_connection_epoch;
    uint32_t session_id;
    uint32_t generation;
} jetkvm_cloud_epoch_state_t;

/** Start a new WSS epoch. Existing session binding remains stale until closed. */
uint32_t jetkvm_cloud_epoch_connected(jetkvm_cloud_epoch_state_t *state);
void jetkvm_cloud_epoch_disconnected(jetkvm_cloud_epoch_state_t *state,
                                     uint32_t connection_epoch);
bool jetkvm_cloud_epoch_is_current_connection(
    const jetkvm_cloud_epoch_state_t *state, uint32_t connection_epoch);
bool jetkvm_cloud_epoch_bind_session(jetkvm_cloud_epoch_state_t *state,
                                     uint32_t connection_epoch,
                                     uint32_t session_id,
                                     uint32_t generation);
bool jetkvm_cloud_epoch_matches_session(
    const jetkvm_cloud_epoch_state_t *state, uint32_t connection_epoch,
    uint32_t session_id, uint32_t generation);
/** Replace only the exact invalidated binding; usable after WSS disconnect. */
bool jetkvm_cloud_epoch_replace_generation(
    jetkvm_cloud_epoch_state_t *state, uint32_t session_id,
    uint32_t invalidated_generation, uint32_t replacement_generation);
void jetkvm_cloud_epoch_clear_session(jetkvm_cloud_epoch_state_t *state,
                                      uint32_t session_id,
                                      uint32_t generation);

/* When the next WSS attempt may start. As upstream's RunWebsocketClient, an
 * attempt needs the network and waits a fixed delay after the previous one
 * failed or ended; the caller owns the clock and the network check. */
typedef struct {
    bool pending;
    int64_t not_before_us;
} jetkvm_cloud_retry_t;

/** Want an attempt no earlier than delay_ms after now_us. */
void jetkvm_cloud_retry_request(jetkvm_cloud_retry_t *retry, int64_t now_us,
                                uint32_t delay_ms);
bool jetkvm_cloud_retry_due(const jetkvm_cloud_retry_t *retry, int64_t now_us,
                            bool network_up);
void jetkvm_cloud_retry_started(jetkvm_cloud_retry_t *retry);

/** Once an offer is authenticated, any transport setup/signaling failure must
 * force a fresh browser offer through a WSS reconnect. */
bool jetkvm_cloud_core_authenticated_step_requires_reconnect(
    bool identity_authenticated, bool step_succeeded);

const char *jetkvm_cloud_core_status_name(jetkvm_cloud_core_status_t status);

#ifdef __cplusplus
}
#endif
