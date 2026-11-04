/* sctp interface.
 * libpeer foundation: Copyright (c) 2021 sepfy.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#ifndef ESP_PEER_SCTP_H
#define ESP_PEER_SCTP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "peer_types.h"

typedef enum { PPID_CONTROL=50, PPID_STRING=51, PPID_BINARY=53, PPID_STRING_EMPTY=56,
               PPID_BINARY_EMPTY=57 } sctp_data_ppid_t;
typedef enum { SCTP_ROLE_SERVER, SCTP_ROLE_CLIENT } sctp_role_t;
typedef struct {
    sctp_role_t role;
    uint32_t init_tsn;
    bool ack_one_frame, manual_ch_create;
    int send_cache_size, recv_cache_size, buf_time_out;
    bool reliable_transport;
    void (*on_connected)(bool, void *);
    void (*on_open)(esp_peer_data_channel_info_t *, void *);
    void (*on_message)(sctp_data_ppid_t, uint16_t, uint8_t *, int, void *);
    void (*on_close)(esp_peer_data_channel_info_t *, void *);
    int (*write_packet)(void *, const uint8_t *, int);
    void *ctx;
} sctp_cfg_t;
typedef void *sctp_handle_t;

_Static_assert(sizeof(sctp_cfg_t) == 52, "vendor SCTP config size");
_Static_assert(offsetof(sctp_cfg_t, on_connected) == 28, "vendor SCTP callback offset");

extern bool sctp_tracing;
sctp_handle_t sctp_create(sctp_cfg_t *cfg);
void sctp_destroy(sctp_handle_t h);
void sctp_query(sctp_handle_t h);
void sctp_main_loop(sctp_handle_t h);
void sctp_incoming_data(sctp_handle_t h, const uint8_t *buf, int len);
int sctp_outgoing_data(sctp_handle_t h, uint8_t *buf, int len, uint16_t stream_id, sctp_data_ppid_t ppid);
int sctp_create_channel(sctp_handle_t h, esp_peer_data_channel_cfg_t *cfg);
int sctp_close_channel(sctp_handle_t h, const char *label);
int sctp_is_connected(sctp_handle_t h);
int sctp_check_data(sctp_handle_t h, const uint8_t *buf, int len);
void sctp_do_sack(sctp_handle_t h);
void sctp_show_details(bool enable);
bool sctp_details_enabled(void);
#endif
