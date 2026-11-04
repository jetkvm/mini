/* agent interface.
 * Upstream lineage: sepfy/libpeer, Copyright (c) 2020 sepfy.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#ifndef ESP_PEER_INTERNAL_AGENT_H
#define ESP_PEER_INTERNAL_AGENT_H
#include "ice.h"
typedef struct agent_t *agent_handle_t;
typedef enum { AGENT_MODE_CONTROLLING, AGENT_MODE_CONTROLLED } agent_mode_t;
typedef struct {
 agent_mode_t mode;
 bool ipv6_support, tcp_support;
 uint8_t max_candidates, max_tcp_connections, alive_binding_retries;
 bool ice_use_lite_mode, turn_skip_cert_verify;
} agent_cfg_t;
typedef struct {
 bool valid;
 uint8_t local_candidate_type, local_transport, pair_type;
} agent_ice_selected_summary_t;
_Static_assert(sizeof(agent_cfg_t)==12,"vendor agent configuration size");
_Static_assert(sizeof(agent_ice_selected_summary_t)==4,"vendor selected summary size");
/* Application integration points. The caller holds the receive lock. */
void *jetkvm_ice_find_pair(void *agent, const esp_peer_addr_t *source);
void jetkvm_ice_add_remote(void *agent, char *candidate, char *end);
void jetkvm_ice_pair_candidates(void *agent);
void *jetkvm_ice_authenticated_pair(void *agent, const esp_peer_addr_t *source,
                                  const uint8_t *wire, size_t size);

agent_handle_t agent_init(agent_cfg_t *cfg);
void agent_set_recv_timeout(agent_handle_t agent, uint16_t timeout);
void agent_change_mode(agent_handle_t agent, agent_mode_t mode);
int agent_add_tcp_stun_server(agent_handle_t agent, const char *urls, const char *username, const char *credential);
void agent_gather_candidate(agent_handle_t agent, const char *urls, const char *username, const char *credential);
int agent_get_local_description(agent_handle_t agent, char *description, int length);
int agent_send(agent_handle_t agent, const uint8_t *buf, int len);
int agent_send_high_priority(agent_handle_t agent, const uint8_t *buf, int len);
int agent_send_to_port(agent_handle_t agent, int port, const uint8_t *buf, int len);
int agent_send_bind_directly(agent_handle_t agent);
void agent_allow_recv_dump(bool enable);
int agent_recv(agent_handle_t agent, uint8_t *buf, int len, bool no_wait);
int agent_set_remote_description(agent_handle_t agent, char *description);
int agent_get_remote_ice(agent_handle_t agent, const char **ufrag, const char **pwd);
int agent_select_candidate_pair(agent_handle_t agent);
bool agent_connectivity_check(agent_handle_t agent);
int agent_get_paired(agent_handle_t agent, esp_peer_addr_t *addr);
int agent_get_ice_selected_summary(agent_handle_t agent, agent_ice_selected_summary_t *out);
bool agent_is_tcp_connected(agent_handle_t agent);
bool agent_is_relay_connection(agent_handle_t agent);
void agent_force_ipv6_only(bool ipv6_only);
void agent_force_relay(bool relay);
void agent_set_tcp_only(bool tcp_only);
int agent_set_ice_policy(agent_handle_t agent, bool relay_only);
int agent_check_alive(agent_handle_t agent);
void agent_disconnect(agent_handle_t agent);
void agent_deinit(agent_handle_t agent);
#endif
