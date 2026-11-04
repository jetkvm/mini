#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hard protocol limits.  They bound both attacker-controlled allocations and
 * cJSON/mbedTLS work before a message reaches the WebRTC implementation.
 */
#define JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE       65536U
#define JETKVM_CLOUD_SIGNAL_MAX_SD_JSON_SIZE       49152U
#define JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE           32768U
#define JETKVM_CLOUD_SIGNAL_MAX_IP_SIZE            128U
#define JETKVM_CLOUD_SIGNAL_MAX_OIDC_GOOGLE_SIZE   16384U
#define JETKVM_CLOUD_SIGNAL_MAX_ICE_SERVERS        16U
#define JETKVM_CLOUD_SIGNAL_MAX_ICE_SERVER_SIZE    1024U
#define JETKVM_CLOUD_SIGNAL_MAX_CANDIDATE_SIZE     2048U
#define JETKVM_CLOUD_SIGNAL_MAX_SDP_MID_SIZE       64U
#define JETKVM_CLOUD_SIGNAL_MAX_USERNAME_SIZE      256U
#define JETKVM_CLOUD_SIGNAL_MAX_JSON_DEPTH         8U
#define JETKVM_CLOUD_SIGNAL_MAX_WIRE_BUFFER_SIZE    \
    (JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE + 1U)

typedef enum {
    JETKVM_CLOUD_SIGNAL_OK = 0,
    JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT,
    JETKVM_CLOUD_SIGNAL_MESSAGE_TOO_LARGE,
    JETKVM_CLOUD_SIGNAL_INVALID_JSON,
    JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE,
    JETKVM_CLOUD_SIGNAL_INVALID_BASE64,
    JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION,
    JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED,
    JETKVM_CLOUD_SIGNAL_NO_MEMORY,
    JETKVM_CLOUD_SIGNAL_OUTPUT_TOO_SMALL,
} jetkvm_cloud_signal_status_t;

typedef struct {
    char *data;
    size_t length;
} jetkvm_cloud_signal_owned_string_t;

/*
 * Successful parse results own every pointer they contain.  Initialize them
 * to zero and release them with the matching free helper.  On parse failure,
 * the result is reset to the all-zero state and owns nothing.
 */
typedef struct {
    jetkvm_cloud_signal_owned_string_t sdp;
    jetkvm_cloud_signal_owned_string_t ip;
    jetkvm_cloud_signal_owned_string_t oidc_google;
    jetkvm_cloud_signal_owned_string_t *ice_servers;
    size_t ice_server_count;
    bool has_ip;
    bool has_oidc_google;
    bool has_ice_servers;
} jetkvm_cloud_signal_offer_t;

typedef struct {
    jetkvm_cloud_signal_owned_string_t candidate;
    jetkvm_cloud_signal_owned_string_t sdp_mid;
    jetkvm_cloud_signal_owned_string_t username_fragment;
    uint16_t sdp_mline_index;
    bool has_username_fragment;
} jetkvm_cloud_signal_candidate_t;

/* A non-owning input view used when emitting a local ICE candidate. */
typedef struct {
    const char *candidate;
    size_t candidate_length;
    const char *sdp_mid;
    size_t sdp_mid_length;
    uint16_t sdp_mline_index;
    const char *username_fragment;
    size_t username_fragment_length;
    bool has_username_fragment;
} jetkvm_cloud_signal_candidate_view_t;

jetkvm_cloud_signal_status_t jetkvm_cloud_signal_parse_offer(
    const uint8_t *message, size_t message_length,
    jetkvm_cloud_signal_offer_t *offer);

jetkvm_cloud_signal_status_t jetkvm_cloud_signal_parse_candidate(
    const uint8_t *message, size_t message_length,
    jetkvm_cloud_signal_candidate_t *candidate);

void jetkvm_cloud_signal_offer_free(jetkvm_cloud_signal_offer_t *offer);
void jetkvm_cloud_signal_candidate_free(
    jetkvm_cloud_signal_candidate_t *candidate);

/*
 * Encoding is transactional: output_length is zero on failure and output is
 * not modified unless the complete NUL-terminated envelope fits.
 */
jetkvm_cloud_signal_status_t jetkvm_cloud_signal_encode_answer(
    const char *sdp, size_t sdp_length, char *output, size_t output_capacity,
    size_t *output_length);

jetkvm_cloud_signal_status_t jetkvm_cloud_signal_encode_candidate(
    const jetkvm_cloud_signal_candidate_view_t *candidate, char *output,
    size_t output_capacity, size_t *output_length);

const char *jetkvm_cloud_signal_status_name(
    jetkvm_cloud_signal_status_t status);

#ifdef __cplusplus
}
#endif
