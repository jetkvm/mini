/* Internal HTTP/peer boundary. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WEBRTC_SIGNAL_NONE = 0,
    WEBRTC_SIGNAL_SDP,
    WEBRTC_SIGNAL_CANDIDATE,
    WEBRTC_SIGNAL_BYE,
} webrtc_signal_type_t;

typedef struct {
    webrtc_signal_type_t type;
    uint32_t session_id;
    uint32_t generation;
    char *data;
    size_t size;
    /* The queue owns one control-arena allocation containing this struct and
     * its trailing NUL-terminated payload.  session_poll copies these fields
     * by value but keeps allocation opaque until signal_event_release(). */
    void *allocation;
    size_t budget_bytes;
} webrtc_signal_event_t;

esp_err_t webrtc_transport_http_start(void);

/* Called on the peer owner only after successful SRTCP authentication. */
void webrtc_transport_on_rtcp(const uint8_t *data, size_t size);
void webrtc_transport_http_stop(void);

esp_err_t webrtc_transport_session_create(uint32_t access_epoch, bool sender_playout_delay,
                                          uint32_t *session_id,
                                          uint32_t *generation);
esp_err_t webrtc_transport_websocket_session_create(uint32_t access_epoch, uint32_t *session_id,
                                                    uint32_t *generation);
esp_err_t webrtc_transport_session_poll(uint32_t session_id, webrtc_signal_event_t *event,
                                        uint32_t *current_generation);
esp_err_t webrtc_transport_websocket_session_poll(uint32_t session_id,
                                                  webrtc_signal_event_t *event,
                                                  uint32_t *current_generation);
bool webrtc_transport_session_superseded(uint32_t session_id);
esp_err_t webrtc_transport_session_submit(uint32_t session_id, uint32_t generation,
                                          webrtc_signal_type_t type,
                                          const char *data, size_t size);
void webrtc_transport_session_close(uint32_t session_id);
void webrtc_transport_signal_event_release(webrtc_signal_event_t *event);
void webrtc_transport_signal_record_drop(void);
const char *webrtc_transport_peer_state_name(int state);

/* Strong policies implemented over esp_peer's open UDP transport hooks. */
void webrtc_peer_udp_timeout_override_init(void);
int webrtc_peer_udp_handshake_timeout_ms(void);
void webrtc_peer_udp_rx_slice_begin(bool bounded, uint32_t max_attempts);
/* Ends the slice. Returns whether it refused a receive attempt, so packets
 * may still wait in the socket. *attempts_left gets the attempts that the
 * slice had left when it stopped at a keyboard change, and 0 when it did
 * not stop there. */
bool webrtc_peer_udp_rx_slice_end(uint32_t *attempts_left);
/* The keyboard state on the host changed and the peer task has not yet
 * sent the change to the browser. */
bool webrtc_transport_keyboard_change_pending(void);

/* lwIP has put a UDP datagram into its socket: ends the peer task's sleep. */
void webrtc_transport_datagram_arrived(void);

void webrtc_transport_revoke_local(void);
