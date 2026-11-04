/* agent implementation.
 * Upstream lineage: sepfy/libpeer, Copyright (c) 2020 sepfy.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#include "utils.h"
#include "peer_platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "agent.h"
#include "stun.h"
#include <pthread.h>
#include "udp.h"
#include "tcp.h"
#include "tls.h"

/* STUN/TURN server replies use the application's handshake receive policy. */
extern int jetkvm_ice_server_recv(udp_socket_t *, esp_peer_addr_t *, uint8_t *, int, bool);

typedef enum { ICE_CANDIDATE_STATE_FROZEN, ICE_CANDIDATE_STATE_WAITING,
 ICE_CANDIDATE_STATE_INPROGRESS, ICE_CANDIDATE_STATE_SUCCEEDED,
 ICE_CANDIDATE_STATE_FAILED } ice_candidate_state_t;
typedef struct {
 ice_candidate_type_t type; ice_candidate_state_t state;
 ice_candidate_t *local, *remote;
 time_t nominated_time; uint32_t last_bind_time; uint64_t priority;
 bool peer_connected, local_connected, prepared, channel_binding;
 uint32_t binding_transaction[3], channel_transaction[3];
} ice_candidate_pair_t;
struct agent_t {
 agent_mode_t mode;
 bool ipv6_support, tcp_support, relay_only, ice_use_lite_mode, turn_skip_cert_verify;
 uint8_t max_candidates, max_tcp_connections, max_paired_candidates, alive_binding_retries;
 char *remote_ufrag, *remote_upwd;
 char local_ufrag[65], local_upwd[65];
 ice_candidate_t *local_candidates, *remote_candidates;
 ice_candidate_pair_t *candidate_pairs, **sort_pairs, *selected_pair;
 uint8_t candidate_pairs_num, local_candidates_count, remote_candidates_count;
 udp_socket_t udp_socket;
 tcp_connections_handle_t tcp; tls_connections_handle_t tls_turn;
 bool tcp_turn_tls; char turn_tls_hostname[64];
 pthread_mutex_t mutex, recv_mutex;
 int stun_relay; ice_candidate_state_t stun_state; udp_socket_t stun_sock;
 esp_peer_addr_t stun_addr, tcp_turn_addr;
 bool tcp_turn_connected; char *stun_user, *stun_psw;
 uint16_t stun_channel_num; bool use_stun_relay; char realm[64], nonce[96];
 stun_msg_t send_msg, recv_msg;
 uint32_t last_stun_binding_time, last_peer_binding_time;
 uint8_t binding_not_response;
 uint32_t last_fresh_time, last_permission_time, last_channel_bind_time;
 uint8_t recv_buf[1400]; uint16_t recv_timeout; uint32_t relay_ready_time;
};
typedef struct agent_t agent_t;
_Static_assert(sizeof(ice_candidate_pair_t)==72,"vendor pair size");
_Static_assert(sizeof(udp_socket_t)==48,"vendor socket size");
_Static_assert(sizeof(agent_t)==5016,"vendor agent size");
_Static_assert(offsetof(agent_t,udp_socket)==184,"vendor socket offset");
_Static_assert(offsetof(agent_t,send_msg)==592,"vendor send message offset");
_Static_assert(offsetof(agent_t,relay_ready_time)==5012,"vendor relay timing offset");

static bool force_relay, force_ipv6_only, force_tcp_only, allow_dump_recv;
#define PRIVATE static

#define LOGE(format, ...) AGENT_LOG_LEVEL(1, format, ##__VA_ARGS__)
#define LOGW(format, ...) AGENT_LOG_LEVEL(2, format, ##__VA_ARGS__)
#define AGENT_LOG_LEVEL(level, format, ...) do { \
    if (((level) & 7) == 1) { \
        esp_log((esp_log_config_t){.data=1}, "AGENT", "E (%lu) %s: " format "\n", esp_log_timestamp(), "AGENT", ##__VA_ARGS__); \
    } else if (((level) & 7) == 2) { \
        esp_log((esp_log_config_t){.data=2}, "AGENT", "W (%lu) %s: " format "\n", esp_log_timestamp(), "AGENT", ##__VA_ARGS__); \
    } else if (((level) & 7) == 4) { \
        esp_log((esp_log_config_t){.data=4}, "AGENT", "D (%lu) %s: " format "\n", esp_log_timestamp(), "AGENT", ##__VA_ARGS__); \
    } else if (((level) & 7) == 5) { \
        esp_log((esp_log_config_t){.data=5}, "AGENT", "V (%lu) %s: " format "\n", esp_log_timestamp(), "AGENT", ##__VA_ARGS__); \
    } else { \
        esp_log((esp_log_config_t){.data=3}, "AGENT", "I (%lu) %s: " format "\n", esp_log_timestamp(), "AGENT", ##__VA_ARGS__); \
    } \
} while (0)
#define LOGI(format, ...) AGENT_LOG_LEVEL(3, format, ##__VA_ARGS__)

/* Packet dispatch and keepalive helpers. */

/* This private packet view permits unaligned transport buffers; the shared
 * stun_header_t describes the naturally aligned message buffer. */
typedef struct __attribute__((packed)) {uint16_t type,length;uint32_t magic_cookie;uint32_t transaction_id[3];} agent_stun_header_t;

/* Internal helper declarations. */
PRIVATE bool agent_turn_stream_is_tls(const agent_t *agent);
PRIVATE bool agent_pair_exists(agent_t *agent, ice_candidate_t *local, ice_candidate_t *remote);
PRIVATE bool is_channel_data(agent_t *agent,uint8_t *buf,int len);
PRIVATE bool agent_nominate_timeout(ice_candidate_pair_t *pair,time_t t);
PRIVATE bool agent_is_connected(agent_t *agent);
PRIVATE int agent_tcp_ensure(agent_t *agent);
PRIVATE void agent_clear_remote_ice(agent_t *agent);
PRIVATE int agent_create_host_addr(agent_t *agent,uint16_t port,esp_peer_addr_t *host_addr);
PRIVATE void agent_add_local_candidate(agent_t *agent,ice_candidate_type_t type,esp_peer_addr_t *addr);
PRIVATE void agent_add_local_candidate_with_transport(agent_t *agent,ice_candidate_type_t type,ice_candidate_transport_t transport,esp_peer_addr_t *addr);
PRIVATE void agent_gather_tcp_host_candidate(agent_t *agent);
PRIVATE uint32_t get_cur_time(void);
PRIVATE bool agent_relay_should_defer(agent_t *agent);
PRIVATE int agent_send_allocate(agent_t *agent,bool init,stun_msg_t *recv_msg,bool keep_id);
PRIVATE int agent_update_credential(agent_t *agent,stun_msg_t *recv_msg);
PRIVATE int agent_recv_while_parse(agent_t *agent,esp_peer_addr_t *dest,stun_msg_t *recv_msg);
PRIVATE int agent_binding_while_allocate(agent_t *agent,const char *username,const char *credential);
PRIVATE int agent_create_bind_addr(agent_t *agent,esp_peer_addr_t *serv_addr,const char *username,const char *credential);
PRIVATE int agent_parse_after_send(agent_t *agent,stun_msg_t *send_msg,stun_msg_t *recv_msg);
PRIVATE int agent_tls_turn_ensure(agent_t *agent,const char *tls_hostname);
PRIVATE int agent_connect_tls_turn(agent_t *agent,esp_peer_addr_t *addr,const char *tls_hostname);
PRIVATE int agent_connect_tcp_turn(agent_t *agent,esp_peer_addr_t *addr);
PRIVATE int agent_turn_send_raw_to(agent_t *agent,const uint8_t *buf,int len);
PRIVATE int agent_send_tcp_allocate(agent_t *agent,bool init,bool keep_id);
PRIVATE int agent_send_turn_data(agent_t *agent,ice_candidate_t *remote,const uint8_t *buf,int len,uint8_t prio);
PRIVATE int agent_send_stun_to_turn(agent_t *agent,ice_candidate_transport_t transport,stun_msg_t *msg);
PRIVATE int agent_send_fresh(agent_t *agent,uint32_t life_time);
PRIVATE int agent_turn_recv_raw_from(agent_t *agent,esp_peer_addr_t *addr,uint8_t *buf,int buf_len,bool no_wait);
PRIVATE int agent_tcp_recv_while_parse(agent_t *agent,esp_peer_addr_t *dest,stun_msg_t *recv_msg);
PRIVATE int agent_gather_tcp_turn_candidate(agent_t *agent,esp_peer_addr_t *addr,const char *username,const char *credential,bool use_tls,const char *tls_hostname);
PRIVATE int agent_send_permission(agent_t *agent,ice_candidate_t *remote,bool send_only);
PRIVATE void agent_candidate_prepare(agent_t *agent);
PRIVATE tcp_connections_handle_t agent_take_tcp(agent_t *agent);
PRIVATE tls_connections_handle_t agent_take_tls(agent_t *agent);
PRIVATE int agent_tcp_send_packet(agent_t *agent,esp_peer_addr_t *addr,const uint8_t *buf,int len,uint8_t prio);
PRIVATE int agent_send_with_prio(agent_handle_t agent,const uint8_t *buf,int len,uint8_t prio);
PRIVATE int agent_build_bind_request(agent_t *agent,stun_msg_t *msg,ice_candidate_pair_t *pair);
PRIVATE int agent_send_indication(agent_t *agent,ice_candidate_pair_t *pair);
PRIVATE int agent_send_bind_request(agent_t *agent,stun_msg_t *send_msg,ice_candidate_pair_t *pair);
PRIVATE void agent_build_binding_response(agent_t *agent,ice_candidate_pair_t *pair,stun_msg_t *stun_msg,stun_msg_t *resp);
PRIVATE int agent_response_indication(agent_t *agent,ice_candidate_pair_t *pair,stun_msg_t *stun_msg);
PRIVATE int agent_send_channel_bind(agent_t *agent,ice_candidate_pair_t *pair,bool refresh);
PRIVATE void agent_select_pair_try_select(agent_t *agent,ice_candidate_pair_t *pair,bool use_candidate);
PRIVATE int agent_recv_tcp_packet(agent_t *agent,esp_peer_addr_t *addr,uint8_t *buf,int len,bool no_wait);
PRIVATE void dump_recv_data(esp_peer_addr_t *addr,uint8_t *data,int len);
PRIVATE bool agent_same_addr(const esp_peer_addr_t *a,const esp_peer_addr_t *b);
PRIVATE bool agent_is_turn_server_addr(agent_t *agent,esp_peer_addr_t *addr);
PRIVATE void agent_process_stun_request(agent_t *agent, esp_peer_addr_t *remote, stun_msg_t *stun_msg);
PRIVATE ice_candidate_pair_t *find_pair_by_transaction(agent_t *agent,uint32_t *id);
PRIVATE ice_candidate_pair_t *find_pair_by_channel_transaction(agent_t *agent,uint32_t *id);
PRIVATE void agent_process_stun_response(agent_t *agent, esp_peer_addr_t *remote, stun_msg_t *stun_msg);
PRIVATE int agent_recv_one_packet(agent_t *agent,uint8_t *buf,int len,bool no_wait,bool *is_stun);
PRIVATE int agent_recv_all(agent_t *agent,uint8_t *buf,int len);
PRIVATE int agent_set_remote_ice_cred(char **dst,const char *src,size_t max_len);
PRIVATE bool agent_have_ongoing_nominate(agent_t *agent);
PRIVATE int agent_check_nominates(agent_t *agent);
agent_handle_t agent_init(agent_cfg_t *cfg);
void agent_set_recv_timeout(agent_handle_t agent,uint16_t timeout);
void agent_change_mode(agent_handle_t agent,agent_mode_t mode);
int agent_add_tcp_stun_server(agent_handle_t agent,const char *urls,const char *username,const char *credential);
void agent_gather_candidate(agent_handle_t agent,const char *urls,const char *username,const char *credential);
int agent_get_local_description(agent_handle_t agent,char *description,int length);
int agent_send(agent_handle_t agent,const uint8_t *buf,int len);
int agent_send_high_priority(agent_handle_t agent,const uint8_t *buf,int len);
int agent_send_to_port(agent_handle_t agent,int port,const uint8_t *buf,int len);
int agent_send_bind_directly(agent_t *agent);
void agent_allow_recv_dump(bool enable);
int agent_recv(agent_handle_t agent,uint8_t *buf,int len,bool no_wait);
int agent_set_remote_description(agent_handle_t agent,char *description);
int agent_get_remote_ice(agent_handle_t agent,const char **ufrag,const char **pwd);
int agent_select_candidate_pair(agent_handle_t agent);
bool agent_connectivity_check(agent_handle_t agent);
int agent_get_paired(agent_handle_t agent,esp_peer_addr_t *addr);
int agent_get_ice_selected_summary(agent_handle_t agent,agent_ice_selected_summary_t *out);
bool agent_is_tcp_connected(agent_handle_t agent);
bool agent_is_relay_connection(agent_handle_t agent);
void agent_force_ipv6_only(bool ipv6_only);
void agent_force_relay(bool relay);
void agent_set_tcp_only(bool tcp_only);
int agent_set_ice_policy(agent_handle_t agent,bool relay_only);
int agent_check_alive(agent_handle_t agent);
void agent_disconnect(agent_handle_t agent);
void agent_deinit(agent_handle_t agent);

PRIVATE bool agent_turn_stream_is_tls(const agent_t *agent) {
 return agent->tcp_turn_connected && agent->tcp_turn_tls && agent->tls_turn;
}

PRIVATE bool agent_pair_exists(agent_t *agent, ice_candidate_t *local, ice_candidate_t *remote) {
 for (int i=0;i<agent->candidate_pairs_num;i++) {
  ice_candidate_pair_t *pair=&agent->candidate_pairs[i];
  if (pair->remote==remote && pair->local==local) return true;
 }
 return false;
}

void jetkvm_ice_pair_candidates(void *handle) {
 agent_t *agent=handle;
 int i,j,n;
 if (!agent->local_candidates_count || !agent->remote_candidates_count) return;
 for(i=0;i<agent->local_candidates_count;i++) {
  for(j=0;j<agent->remote_candidates_count;j++) {
   ice_candidate_t *remote=&agent->remote_candidates[j];
   if (force_tcp_only && remote->transport!=ICE_CANDIDATE_TRANSPORT_TCP) continue;
   if (agent->local_candidates[i].type==remote->type &&
       agent->local_candidates[i].addr.family==remote->addr.family &&
       agent->local_candidates[i].transport==remote->transport &&
       agent->candidate_pairs_num<agent->max_paired_candidates &&
       !agent_pair_exists(agent,&agent->local_candidates[i],remote)) {
    ice_candidate_pair_t *pair=&agent->candidate_pairs[agent->candidate_pairs_num];
    pair->type=remote->type;pair->local=&agent->local_candidates[i];pair->remote=remote;
    pair->priority=(uint64_t)agent->local_candidates[i].priority+remote->priority;
    pair->state=ICE_CANDIDATE_STATE_FROZEN;
    agent->sort_pairs[agent->candidate_pairs_num]=pair;agent->candidate_pairs_num++;
   }
  }
 }
 n=agent->candidate_pairs_num;
 for(i=0;i<n-1;i++) {
  bool swapped=false;
  for(j=0;j<n-i-1;j++) {
   if (agent->sort_pairs[j]->priority<agent->sort_pairs[j+1]->priority) {
    ice_candidate_pair_t *pair=agent->sort_pairs[j];agent->sort_pairs[j]=agent->sort_pairs[j+1];agent->sort_pairs[j+1]=pair;
    swapped=true;
   }
  }
  if (!swapped) break;
 }
}

PRIVATE bool is_channel_data(agent_t *agent,uint8_t *buf,int len) {
 return len>4 && *(uint16_t *)buf==agent->stun_channel_num;
}

PRIVATE bool agent_nominate_timeout(ice_candidate_pair_t *pair,time_t t) {
 return pair->state==ICE_CANDIDATE_STATE_INPROGRESS && t>pair->nominated_time+9;
}

PRIVATE bool agent_is_connected(agent_t *agent) {
 return agent->selected_pair && agent->selected_pair->local_connected && agent->selected_pair->peer_connected;
}

PRIVATE int agent_tcp_ensure(agent_t *agent) {
 if (!agent->tcp_support) return -4;
 if (!agent->tcp) {
   tcp_connections_cfg_t cfg={.ipv6_support=agent->ipv6_support,.max_connections=agent->max_tcp_connections,
    .connect_timeout_ms=30000,.send_timeout_ms=1000,.recv_timeout_ms=agent->recv_timeout?agent->recv_timeout:150};
   agent->tcp=tcp_connections_open(&cfg);
 return agent->tcp ? 0 : -2;
 }
 return 0;
}

PRIVATE void agent_clear_remote_ice(agent_t *agent) {
 if (agent->remote_ufrag) {media_lib_free(agent->remote_ufrag);agent->remote_ufrag=NULL;}
 if (agent->remote_upwd) {media_lib_free(agent->remote_upwd);agent->remote_upwd=NULL;}
}

PRIVATE int agent_create_host_addr(agent_t *agent,uint16_t port,esp_peer_addr_t *host_addr) {
 if (ports_get_host_addr(host_addr)) {host_addr->port=port;return 0;}return -1;
}

PRIVATE void agent_add_local_candidate(agent_t *agent,ice_candidate_type_t type,esp_peer_addr_t *addr) {
 if (agent->local_candidates_count<agent->max_candidates) {
  ice_candidate_create(&agent->local_candidates[agent->local_candidates_count],agent->local_candidates_count,type,addr);
  agent->local_candidates_count++;
 }
}

PRIVATE void agent_add_local_candidate_with_transport(agent_t *agent,ice_candidate_type_t type,ice_candidate_transport_t transport,esp_peer_addr_t *addr) {
 if (agent->local_candidates_count<agent->max_candidates) {
  ice_candidate_create_with_transport(&agent->local_candidates[agent->local_candidates_count],agent->local_candidates_count,type,transport,addr);
  agent->local_candidates_count++;
 }
}

PRIVATE void agent_gather_tcp_host_candidate(agent_t *agent) {
 if (!agent->tcp_support || agent->relay_only) return;
 if (agent_tcp_ensure(agent)!=0) return;
 esp_peer_addr_t tcp_addr={0};tcp_addr.family=2;
 if (tcp_connections_bind(agent->tcp,&tcp_addr)!=0) {LOGW("Failed to bind TCP host candidate");return;}
 if (tcp_connections_start_server(agent->tcp)!=0) {LOGW("Failed to start TCP host candidate server");return;}
 if (agent_create_host_addr(agent,tcp_addr.port,&tcp_addr)==0)
  agent_add_local_candidate_with_transport(agent,ICE_CANDIDATE_TYPE_HOST,ICE_CANDIDATE_TRANSPORT_TCP,&tcp_addr);
}

PRIVATE uint32_t get_cur_time(void) {
 struct timeval tv;
 gettimeofday(&tv,NULL);
 long ms=tv.tv_sec*1000+tv.tv_usec/1000;
 return ms;
}

PRIVATE bool agent_relay_should_defer(agent_t *agent) {
 if (agent->selected_pair) return false;
 if (force_relay || agent->relay_only) return false;
 bool have_local=false;
 for (int i=0;i<agent->local_candidates_count;i++) {
  if (agent->local_candidates[i].type!=ICE_CANDIDATE_TYPE_RELAY) {have_local=true;break;}
 }
 if (!have_local) return false;
 uint32_t now=get_cur_time();
 if (!agent->relay_ready_time) agent->relay_ready_time=now;
 return now<agent->relay_ready_time+800;
}

PRIVATE int agent_send_allocate(agent_t *agent,bool init,stun_msg_t *recv_msg,bool keep_id) {
 stun_msg_t *send_msg=&agent->send_msg;
 LOGI("Send allocate now");
 stun_msg_create(send_msg,STUN_METHOD_ALLOCATE,!keep_id);
 uint32_t attr=17;
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_REQUESTED_TRANSPORT,sizeof(attr),(char*)&attr);
 if (!init) {
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_USERNAME,strlen(agent->stun_user),agent->stun_user);
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_REALM,strlen(agent->realm),agent->realm);
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_NONCE,strlen(agent->nonce),agent->nonce);
  stun_msg_finish(send_msg,STUN_CREDENTIAL_LONG_TERM,agent->stun_psw,strlen(agent->stun_psw));
 } else stun_msg_finish(send_msg,STUN_CREDENTIAL_NONE,NULL,0);
 int ret=udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_msg->buf,send_msg->size);
 return ret>0?0:ret;
}

PRIVATE int agent_update_credential(agent_t *agent,stun_msg_t *recv_msg) {
 if (recv_msg->stunclass==STUN_CLASS_ERROR && (recv_msg->err_code==401 || recv_msg->err_code==438)) {
  if (recv_msg->realm_size) {
   if (recv_msg->realm_size+1>sizeof(agent->realm)) {LOGE("Stun realm size %d over limit",recv_msg->realm_size);return -1;}
   memcpy(agent->realm,recv_msg->realm,recv_msg->realm_size);agent->realm[recv_msg->realm_size]=0;
  }
  if (recv_msg->nonce_size) {
   if (recv_msg->nonce_size+1>sizeof(agent->nonce)) {LOGE("Stun nonce size %d over limit",recv_msg->nonce_size);return -1;}
   memcpy(agent->nonce,recv_msg->nonce,recv_msg->nonce_size);agent->nonce[recv_msg->nonce_size]=0;
  }
 }
 return 0;
}

PRIVATE int agent_recv_while_parse(agent_t *agent,esp_peer_addr_t *dest,stun_msg_t *recv_msg) {
 int ret=jetkvm_ice_server_recv(&agent->stun_sock,dest,recv_msg->buf,sizeof(recv_msg->buf),false);
 if (ret<=0) {LOGE("%d Failed to receive request\n",agent->mode);return -1;}
 stun_parse_msg_buf(recv_msg);return agent_update_credential(agent,recv_msg);
}

PRIVATE int agent_binding_while_allocate(agent_t *agent,const char *username,const char *credential) {
 LOGI("Send STUN binding request");
 stun_msg_t *send_msg=&agent->send_msg,*recv_msg=&agent->recv_msg;
 int retry_count=0;
 esp_peer_addr_t mapped_addr={0};bool reflex_add=false;
 while(retry_count++<5) {
  stun_msg_create(send_msg,STUN_METHOD_BINDING,false);
  stun_msg_finish(send_msg,STUN_CREDENTIAL_NONE,NULL,0);
  agent->last_stun_binding_time=get_cur_time();
  int ret=udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_msg->buf,send_msg->size);
  if(ret<0)return ret;
  if(username){ret=agent_send_allocate(agent,true,recv_msg,false);if(ret<0)return ret;}
  memset(recv_msg,0,sizeof(*recv_msg));
  esp_peer_addr_t dest;
  ret=agent_recv_while_parse(agent,&dest,recv_msg);
  if(ret!=0)continue;
  if(recv_msg->mapped_addr.port) {
   mapped_addr=recv_msg->mapped_addr;
   if(!reflex_add){
    if(!agent->relay_only)agent_add_local_candidate(agent,ICE_CANDIDATE_TYPE_SRFLX,&mapped_addr);
    reflex_add=true;
   }
  }
  if(username && recv_msg->stunmethod==STUN_METHOD_BINDING) {
   if(agent_recv_while_parse(agent,&dest,recv_msg)!=0)continue;
   if(recv_msg->mapped_addr.port)mapped_addr=recv_msg->mapped_addr;
  }
  agent->stun_user=(char*)username;agent->stun_psw=(char*)credential;
  if(recv_msg->stunmethod==STUN_METHOD_ALLOCATE) {
   int allocate_limit=3;bool resend_allocate=false;
   for(int i=0;i<allocate_limit;i++) {
    if((recv_msg->stunmethod==STUN_METHOD_ALLOCATE && recv_msg->stunclass==STUN_CLASS_ERROR &&
        (recv_msg->err_code==401 || recv_msg->err_code==438)) || resend_allocate) {
     agent->last_fresh_time=get_cur_time();recv_msg->err_code=0;
     ret=agent_send_allocate(agent,false,recv_msg,resend_allocate);
     if(ret<0){LOGE("Failed to send allocate");return ret;}
     ret=agent_recv_while_parse(agent,&dest,recv_msg);
     if(ret!=0){resend_allocate=true;LOGE("Failed to recv retry ret %d\n",ret);continue;}
     if(recv_msg->mapped_addr.port)mapped_addr=recv_msg->mapped_addr;
     if(recv_msg->relayed_addr.port)break;
     if(recv_msg->stunmethod==STUN_METHOD_ALLOCATE)resend_allocate=false;
     else resend_allocate=true;
    }else break;
   }
  }
  if(!mapped_addr.port)continue;
  if(!agent->relay_only && !reflex_add)agent_add_local_candidate(agent,ICE_CANDIDATE_TYPE_SRFLX,&mapped_addr);
  if(recv_msg->relayed_addr.port)agent_add_local_candidate(agent,ICE_CANDIDATE_TYPE_RELAY,&recv_msg->relayed_addr);
  LOGI("%d Get candidate success user:%s psw:%s",agent->mode,username?username:"",credential?credential:"");
  return 0;
 }
 return 0;
}

PRIVATE int agent_create_bind_addr(agent_t *agent,esp_peer_addr_t *serv_addr,const char *username,const char *credential) {
 if(agent->stun_sock.fd<=0) {
  udp_socket_open(&agent->stun_sock,agent->ipv6_support);
  esp_peer_addr_t bind_addr={0};udp_socket_bind(&agent->stun_sock,&bind_addr);
  udp_blocking_timeout(&agent->stun_sock,agent->recv_timeout?agent->recv_timeout:150);
 }
 if(!serv_addr)return 0;
 agent->stun_addr=*serv_addr;return agent_binding_while_allocate(agent,username,credential);
}

PRIVATE int agent_parse_after_send(agent_t *agent,stun_msg_t *send_msg,stun_msg_t *recv_msg) {
 int ret=udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_msg->buf,send_msg->size);
 if (ret<=0) {LOGE("%d Failed to send request\n",agent->mode);return -1;}
 memset(recv_msg,0,sizeof(*recv_msg));
 esp_peer_addr_t dest;
 return agent_recv_while_parse(agent,&dest,recv_msg);
}

PRIVATE int agent_tls_turn_ensure(agent_t *agent,const char *tls_hostname) {
 if (agent->tls_turn) return 0;
 if (!tls_hostname || !tls_hostname[0]) return -1;
 strncpy(agent->turn_tls_hostname,tls_hostname,sizeof(agent->turn_tls_hostname)-1);
 agent->turn_tls_hostname[sizeof(agent->turn_tls_hostname)-1]=0;
 uint16_t io_to=agent->recv_timeout?agent->recv_timeout:150;
 tls_connections_cfg_t tls_cfg={0};
 tls_cfg.ipv6_support=agent->ipv6_support;tls_cfg.max_connections=agent->max_tcp_connections;
 tls_cfg.connect_timeout_ms=30000;tls_cfg.send_timeout_ms=1000;tls_cfg.recv_timeout_ms=io_to;
 tls_cfg.tls_hostname=agent->turn_tls_hostname;tls_cfg.tls_hostname_len=strlen(agent->turn_tls_hostname);
 tls_cfg.skip_cert_verify=agent->turn_skip_cert_verify;
 agent->tls_turn=tls_connections_open(&tls_cfg);
 return agent->tls_turn?0:-2;
}

PRIVATE int agent_connect_tls_turn(agent_t *agent,esp_peer_addr_t *addr,const char *tls_hostname) {
 int ret=agent_tls_turn_ensure(agent,tls_hostname);if(ret)return ret;
 agent->tcp_turn_tls=true;agent->tcp_turn_addr=*addr;
 ret=tls_connections_connect(agent->tls_turn,&agent->tcp_turn_addr);
 if(ret){LOGE("Failed to connect TLS TURN transport");return -6;}
 uint32_t start=get_cur_time();
 while(get_cur_time()>=start && get_cur_time()<start+30000) {
  if(!agent->tls_turn){LOGE("TLS TURN transport aborted by agent_disconnect()/agent_deinit()");return -6;}
  int connected=tls_connections_is_connected(agent->tls_turn,&agent->tcp_turn_addr);
  if(connected<0){LOGE("TLS TURN failed to connect");return -6;}
  if(connected==1){agent->tcp_turn_connected=true;LOGI("TLS TURN transport connected (verify server identity in production)");return 0;}
  media_lib_thread_sleep(10);
 }
 LOGE("TLS TURN transport connection timed out");return -6;
}

PRIVATE int agent_connect_tcp_turn(agent_t *agent, esp_peer_addr_t *addr)
{
    int ret = agent_tcp_ensure(agent);
    if (ret) return ret;
    agent->tcp_turn_tls = false;
    agent->tcp_turn_addr = *addr;
    ret = tcp_connections_connect(agent->tcp, &agent->tcp_turn_addr);
    if (ret) return -6;
    uint32_t start = get_cur_time();
    goto check_connection_deadline;
wait_for_connection: {
        if (!agent->tcp) return -6;
        if (tcp_connections_is_connected(agent->tcp, &agent->tcp_turn_addr)) {
            agent->tcp_turn_connected = true;
            return 0;
        }
        media_lib_thread_sleep(10);
    }
check_connection_deadline:
    if (get_cur_time() >= start && get_cur_time() < start + 30000) {
        goto wait_for_connection;
    }
    return -6;
}

PRIVATE int agent_turn_send_raw_to(agent_t *agent,const uint8_t *buf,int len) {
 if (agent_turn_stream_is_tls(agent)) return tls_connections_send_raw_to(agent->tls_turn,&agent->tcp_turn_addr,buf,len);
 if (agent->tcp) return tcp_connections_send_raw_to(agent->tcp,&agent->tcp_turn_addr,buf,len);
 return -1;
}

PRIVATE int agent_send_tcp_allocate(agent_t *agent,bool init,bool keep_id) {
 stun_msg_t *send_msg=&agent->send_msg;
 LOGI("Send TCP TURN allocate now");
 stun_msg_create(send_msg,STUN_METHOD_ALLOCATE,!keep_id);
 uint32_t attr=17;
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_REQUESTED_TRANSPORT,sizeof(attr),(char*)&attr);
 if (!init) {
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_USERNAME,strlen(agent->stun_user),agent->stun_user);
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_REALM,strlen(agent->realm),agent->realm);
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_NONCE,strlen(agent->nonce),agent->nonce);
  stun_msg_finish(send_msg,STUN_CREDENTIAL_LONG_TERM,agent->stun_psw,strlen(agent->stun_psw));
 } else stun_msg_finish(send_msg,STUN_CREDENTIAL_NONE,NULL,0);
 return agent_turn_send_raw_to(agent,send_msg->buf,send_msg->size)<0?-1:0;
}

PRIVATE int agent_send_turn_data(agent_t *agent,ice_candidate_t *remote,const uint8_t *buf,int len,uint8_t prio) {
 if (!remote || len+64>=sizeof(agent->send_msg.buf)) return -7;
 stun_msg_t *send_msg=&agent->send_msg;
 stun_msg_create(send_msg,STUN_CLASS_INDICATION|STUN_METHOD_SEND,true);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_XOR_PEER_ADDRESS,0,(char*)&remote->addr);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_DATA,len,(char*)buf);
 stun_msg_finish(send_msg,STUN_CREDENTIAL_NONE,NULL,0);
 if (agent->tcp_turn_connected || remote->transport==ICE_CANDIDATE_TRANSPORT_TCP) {
  int ret=agent_turn_send_raw_to(agent,send_msg->buf,send_msg->size);
  return ret>0?len:ret;
 }
 int ret=udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_msg->buf,send_msg->size);
 return ret>0?len:ret;
}

PRIVATE int agent_send_stun_to_turn(agent_t *agent,ice_candidate_transport_t transport,stun_msg_t *msg) {
 if (agent->tcp_turn_connected || transport==ICE_CANDIDATE_TRANSPORT_TCP)
  return agent_turn_send_raw_to(agent,msg->buf,msg->size)<0?-1:0;
 return udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,msg->buf,msg->size)<0?-1:0;
}

PRIVATE int agent_send_fresh(agent_t *agent,uint32_t life_time) {
 if (!agent->local_candidates_count || !agent->stun_psw) return 0;
 stun_msg_t *send_msg=&agent->send_msg;
 stun_msg_create(send_msg,STUN_METHOD_REFRESH,true);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_LIFETIME,sizeof(life_time),(char*)&life_time);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_USERNAME,strlen(agent->stun_user),agent->stun_user);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_NONCE,strlen(agent->nonce),agent->nonce);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_REALM,strlen(agent->realm),agent->realm);
 stun_msg_finish(send_msg,STUN_CREDENTIAL_LONG_TERM,agent->stun_psw,strlen(agent->stun_psw));
 ice_candidate_transport_t transport=ICE_CANDIDATE_TRANSPORT_UDP;
 if (agent->selected_pair) {
  transport=agent->selected_pair->local->transport;
  if (transport!=ICE_CANDIDATE_TRANSPORT_TCP) transport=ICE_CANDIDATE_TRANSPORT_UDP;
 }
 return agent_send_stun_to_turn(agent,transport,send_msg);
}

PRIVATE int agent_turn_recv_raw_from(agent_t *agent,esp_peer_addr_t *addr,uint8_t *buf,int buf_len,bool no_wait) {
 if (agent_turn_stream_is_tls(agent)) return tls_connections_recv_raw_from(agent->tls_turn,addr,buf,buf_len,no_wait);
 if (agent->tcp) return tcp_connections_recv_raw_from(agent->tcp,addr,buf,buf_len,no_wait);
 return -1;
}

PRIVATE int agent_tcp_recv_while_parse(agent_t *agent,esp_peer_addr_t *dest,stun_msg_t *recv_msg) {
 int ret=agent_turn_recv_raw_from(agent,dest,recv_msg->buf,sizeof(recv_msg->buf),false);
 if (ret<=0) {LOGE("%d Failed to receive TCP TURN response",agent->mode);return -1;}
 recv_msg->size=ret;
 stun_parse_msg_buf(recv_msg);return agent_update_credential(agent,recv_msg);
}

PRIVATE int agent_gather_tcp_turn_candidate(agent_t *agent,esp_peer_addr_t *addr,const char *username,const char *credential,bool use_tls,const char *tls_hostname) {
 if(!username || !credential)return -1;
 int ret=use_tls?agent_connect_tls_turn(agent,addr,tls_hostname):agent_connect_tcp_turn(agent,addr);
 if(ret){LOGE("Failed to connect %s TURN server",use_tls?"TLS":"TCP");return ret;}
 agent->stun_user=(char*)username;agent->stun_psw=(char*)credential;
 stun_msg_t *recv_msg=&agent->recv_msg;
 esp_peer_addr_t dest;
 bool resend_allocate=false;
 for(int i=0;i<5;i++) {
  memset(recv_msg,0,sizeof(*recv_msg));
  if(agent_send_tcp_allocate(agent,i==0,resend_allocate)!=0)return -6;
  ret=agent_tcp_recv_while_parse(agent,&dest,recv_msg);
  if(ret!=0){resend_allocate=true;continue;}
  if(recv_msg->stunmethod==STUN_METHOD_ALLOCATE && recv_msg->stunclass==STUN_CLASS_ERROR &&
    (recv_msg->err_code==401 || recv_msg->err_code==438)){resend_allocate=false;continue;}
  if(recv_msg->stunmethod==STUN_METHOD_ALLOCATE && recv_msg->relayed_addr.port) {
   agent_add_local_candidate_with_transport(agent,ICE_CANDIDATE_TYPE_RELAY,ICE_CANDIDATE_TRANSPORT_UDP,&recv_msg->relayed_addr);
   agent->last_fresh_time=get_cur_time();
   LOGI("%d Get TCP TURN relay candidate success",agent->mode);return 0;
  }
  resend_allocate=false;
 }
 return -6;
}

PRIVATE int agent_send_permission(agent_t *agent,ice_candidate_t *remote,bool send_only) {
 if(!agent->stun_user || !agent->stun_psw)return -1;
 stun_msg_t *send_msg=&agent->send_msg,*recv_msg=&agent->recv_msg;
 for(int retry=0;retry<2;retry++) {
  stun_msg_create(send_msg,STUN_METHOD_CREATE_PERMISSION,true);
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_XOR_PEER_ADDRESS,0,(char*)&remote->addr);
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_USERNAME,strlen(agent->stun_user),agent->stun_user);
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_NONCE,strlen(agent->nonce),agent->nonce);
  stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_REALM,strlen(agent->realm),agent->realm);
  stun_msg_finish(send_msg,STUN_CREDENTIAL_LONG_TERM,agent->stun_psw,strlen(agent->stun_psw));
  if(send_only)return agent_send_stun_to_turn(agent,remote->transport,send_msg);
  int ret;
  if(agent->tcp_turn_connected || remote->transport==ICE_CANDIDATE_TRANSPORT_TCP) {
   ret=agent_send_stun_to_turn(agent,remote->transport,send_msg);
   if(ret==0){memset(recv_msg,0,sizeof(*recv_msg));esp_peer_addr_t dest;ret=agent_tcp_recv_while_parse(agent,&dest,recv_msg);}
  }else ret=agent_parse_after_send(agent,send_msg,recv_msg);
  if(ret<0){LOGE("Failed to send Permission Request");return ret;}
  if(recv_msg->stunmethod!=STUN_METHOD_CREATE_PERMISSION)return -1;
  if(recv_msg->stunclass==STUN_CLASS_RESPONSE){agent->last_permission_time=get_cur_time();return 0;}
  if(recv_msg->stunclass!=STUN_CLASS_ERROR)return -1;
  if(recv_msg->err_code!=401 && recv_msg->err_code!=438)return -1;
 }
 LOGE("Permission request failed after auth retry");return -1;
}

PRIVATE void agent_candidate_prepare(agent_t *agent) {
 for(int i=0;i<agent->candidate_pairs_num;i++) {
  ice_candidate_pair_t *pair=agent->sort_pairs[i];
  if (pair->prepared) continue;
  pair->state=ICE_CANDIDATE_STATE_FROZEN;pair->last_bind_time=0;
  if (pair->type==ICE_CANDIDATE_TYPE_RELAY) {
   if (agent->local_candidates_count>1) pair->last_bind_time=get_cur_time()+800;
   agent_send_permission(agent,pair->remote,false);
  } else if (pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP) tcp_connections_connect(agent->tcp,&pair->remote->addr);
  pair->prepared=true;
 }
}

PRIVATE tcp_connections_handle_t agent_take_tcp(agent_t *agent) {
 pthread_mutex_lock(&agent->mutex);
 tcp_connections_handle_t tcp=agent->tcp;agent->tcp=NULL;
 pthread_mutex_unlock(&agent->mutex);return tcp;
}

PRIVATE tls_connections_handle_t agent_take_tls(agent_t *agent) {
 pthread_mutex_lock(&agent->mutex);
 tls_connections_handle_t tls=agent->tls_turn;agent->tls_turn=NULL;
 pthread_mutex_unlock(&agent->mutex);return tls;
}

PRIVATE int agent_tcp_send_packet(agent_t *agent,esp_peer_addr_t *addr,const uint8_t *buf,int len,uint8_t prio) {
 if (!agent->tcp) return -7;
 return tcp_connections_send_to(agent->tcp,addr,buf,len,prio);
}

PRIVATE int agent_send_with_prio(agent_handle_t agent,const uint8_t *buf,int len,uint8_t prio) {
 if (!agent) return -1;
 if (agent->selected_pair && agent->selected_pair->type==ICE_CANDIDATE_TYPE_RELAY && agent->tcp_turn_connected)
  return agent_send_turn_data(agent,agent->selected_pair->remote,buf,len,prio);
 if (agent->use_stun_relay) {
  if (len+4>=sizeof(agent->send_msg.buf)) {LOGE("Drop for size %d over receive buf size %d\n",len,sizeof(agent->send_msg.buf));return -7;}
  uint8_t *send_buf=agent->send_msg.buf;
  *(uint16_t*)send_buf=agent->stun_channel_num;
  *(uint16_t*)(send_buf+2)=__builtin_bswap16(len);
  memcpy(send_buf+4,buf,len);
  int ret=udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_buf,len+4);
  return ret>4?ret-4:ret;
 }
 if (!agent->selected_pair || !agent->stun_sock.fd) return -3;
 if (agent->selected_pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP)
  return agent_tcp_send_packet(agent,&agent->selected_pair->remote->addr,buf,len,prio);
 return udp_socket_sendto(&agent->stun_sock,&agent->selected_pair->remote->addr,buf,len);
}

PRIVATE int agent_build_bind_request(agent_t *agent,stun_msg_t *msg,ice_candidate_pair_t *pair) {
 if (!agent->remote_ufrag || !agent->remote_upwd)return -1;
 stun_msg_create(msg,STUN_METHOD_BINDING,true);
 char username[322];memset(username,0,sizeof(username));
 snprintf(username,sizeof(username),"%s:%s",agent->remote_ufrag,agent->local_ufrag);
 stun_msg_write_attr(msg,STUN_ATTR_TYPE_USERNAME,strlen(username),username);
 stun_msg_write_attr(msg,STUN_ATTR_TYPE_PRIORITY,4,(char*)&pair->local->priority);
 agent_stun_header_t *header=(agent_stun_header_t*)msg->buf;
 memcpy(pair->binding_transaction,header->transaction_id,sizeof(pair->binding_transaction));
 uint64_t tie_breaker=utils_get_timestamp();
 if (agent->mode==AGENT_MODE_CONTROLLING) {
  stun_msg_write_attr(msg,STUN_ATTR_TYPE_ICE_CONTROLLING,8,(char*)&tie_breaker);
  if (agent->selected_pair==pair)stun_msg_write_attr(msg,STUN_ATTR_TYPE_USE_CANDIDATE,0,NULL);
 }else stun_msg_write_attr(msg,STUN_ATTR_TYPE_ICE_CONTROLLED,8,(char*)&tie_breaker);
 stun_msg_finish(msg,STUN_CREDENTIAL_SHORT_TERM,agent->remote_upwd,strlen(agent->remote_upwd));
 if (pair->peer_connected && pair->local_connected)return 0;
 char ip_str[128];
 LOGI("%d %d Send binding request (cand:%d) local%d:%s remote%d:%s id:%08x%08x%08x",agent->mode,(int)(pair-agent->candidate_pairs),pair==agent->selected_pair,pair->local_connected,
 esp_peer_addr_to_str(&pair->local->addr,ip_str),pair->peer_connected,esp_peer_addr_to_str(&pair->remote->addr,ip_str+64),(unsigned int)pair->binding_transaction[0],(unsigned int)pair->binding_transaction[1],(unsigned int)pair->binding_transaction[2]);
 return 0;
}

PRIVATE int agent_send_indication(agent_t *agent,ice_candidate_pair_t *pair) {
 if (pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP)return 0;
 stun_msg_t *send_msg=&agent->send_msg;
 ice_candidate_t *remote=pair->remote;
 stun_msg_create(send_msg,STUN_CLASS_INDICATION|STUN_METHOD_SEND,true);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_XOR_PEER_ADDRESS,0,(char*)&remote->addr);
 stun_msg_t bind_body;
 agent_build_bind_request(agent,&bind_body,pair);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_DATA,bind_body.size,(char*)bind_body.buf);
 stun_msg_finish(send_msg,STUN_CREDENTIAL_NONE,NULL,0);
 if (!pair->local_connected)LOGI("%d send indication bind request\n",agent->mode);
 int ret;
 if (agent->tcp_turn_connected)ret=agent_turn_send_raw_to(agent,send_msg->buf,send_msg->size);
 else ret=udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_msg->buf,send_msg->size);
 if (ret>0) return 0;
 return -1;
}

PRIVATE int agent_send_bind_request(agent_t *agent,stun_msg_t *send_msg,ice_candidate_pair_t *pair) {
 agent_build_bind_request(agent,send_msg,pair);
 if (pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP)
  return agent_tcp_send_packet(agent,&pair->remote->addr,send_msg->buf,send_msg->size,2);
 int ret=udp_socket_sendto(&agent->stun_sock,&pair->remote->addr,send_msg->buf,send_msg->size);
 return ret>0?0:-1;
}

PRIVATE void agent_build_binding_response(agent_t *agent,ice_candidate_pair_t *pair,stun_msg_t *stun_msg,stun_msg_t *resp) {
 stun_msg_create(resp,STUN_CLASS_RESPONSE|STUN_METHOD_BINDING,false);
 agent_stun_header_t *stun_header=(agent_stun_header_t*)stun_msg->buf;
 agent_stun_header_t *header=(agent_stun_header_t*)resp->buf;
 memcpy(header->transaction_id,stun_header->transaction_id,sizeof(header->transaction_id));
 stun_msg_write_attr(resp,STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS,8,(char*)&pair->remote->addr);
 stun_msg_finish(resp,STUN_CREDENTIAL_SHORT_TERM,agent->local_upwd,strlen(agent->local_upwd));
 if (pair->peer_connected && pair->local_connected)return;
 char remote_str[64],local_str[64];
 LOGI("%d %d Send binding response local%d:%s remote%d:%s",agent->mode,(int)(pair-agent->candidate_pairs),pair->local_connected,esp_peer_addr_to_str(&pair->local->addr,local_str),pair->peer_connected,esp_peer_addr_to_str(&pair->remote->addr,remote_str));
}

PRIVATE int agent_response_indication(agent_t *agent,ice_candidate_pair_t *pair,stun_msg_t *stun_msg) {
 bool use_candidate=stun_msg->use_candidate;
 ice_candidate_t *remote=pair->remote;
 stun_msg_t bind_body;
 memset(&bind_body,0,sizeof(bind_body));
 agent_build_binding_response(agent,pair,stun_msg,&bind_body);
 stun_msg_t *send_msg=&agent->send_msg;
 stun_msg_create(send_msg,STUN_CLASS_INDICATION|STUN_METHOD_SEND,true);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_XOR_PEER_ADDRESS,0,(char*)&remote->addr);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_DATA,bind_body.size,(char*)bind_body.buf);
 stun_msg_finish(send_msg,STUN_CREDENTIAL_NONE,NULL,0);
 LOGI("%d send indication bind response\n",agent->mode);
 int ret;
 if (agent->tcp_turn_connected)ret=agent_turn_send_raw_to(agent,send_msg->buf,send_msg->size);
 else ret=udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_msg->buf,send_msg->size);
 if (ret<=0) {LOGE("%d Failed to send STUN Binding Request",agent->mode);return ret;}
 if (use_candidate && agent->mode==AGENT_MODE_CONTROLLED)return agent_send_indication(agent,pair);
 return ret;
}

PRIVATE int agent_send_channel_bind(agent_t *agent,ice_candidate_pair_t *pair,bool refresh) {
 if(pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP)return 0;
 if(agent->stun_state==ICE_CANDIDATE_STATE_SUCCEEDED && !refresh)return 0;
 stun_msg_t *send_msg=&agent->send_msg;
 stun_msg_create(send_msg,STUN_METHOD_CHANNEL_BIND,true);
 agent->stun_channel_num=576;
 agent_stun_header_t *header=(agent_stun_header_t*)send_msg->buf;
 memcpy(pair->channel_transaction,header->transaction_id,sizeof(pair->channel_transaction));
 pair->channel_binding=true;
 char ip_str[64];
 LOGI("%d Start remote %s send Channel Bind Request id:%08x%08x%08x",agent->mode,esp_peer_addr_to_str(&pair->remote->addr,ip_str),(unsigned int)pair->channel_transaction[0],(unsigned int)pair->channel_transaction[1],(unsigned int)pair->channel_transaction[2]);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_XOR_PEER_ADDRESS,0,(char*)&pair->remote->addr);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_CHANNEL_NUM,2,(char*)&agent->stun_channel_num);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_USERNAME,strlen(agent->stun_user),agent->stun_user);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_NONCE,strlen(agent->nonce),agent->nonce);
 stun_msg_write_attr(send_msg,STUN_ATTR_TYPE_REALM,strlen(agent->realm),agent->realm);
 stun_msg_finish(send_msg,STUN_CREDENTIAL_LONG_TERM,agent->stun_psw,strlen(agent->stun_psw));
 int ret;
 if(agent->tcp_turn_connected)ret=agent_turn_send_raw_to(agent,send_msg->buf,send_msg->size);
 else ret=udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_msg->buf,send_msg->size);
 if(ret<0){LOGE("%d Failed to send Data Binding Request",agent->mode);return ret;}
 agent->last_channel_bind_time=get_cur_time();return 0;
}

PRIVATE void agent_select_pair_try_select(agent_t *agent,ice_candidate_pair_t *pair,bool use_candidate) {
 if(!use_candidate)return;
 if(agent->mode==AGENT_MODE_CONTROLLED && pair->type!=ICE_CANDIDATE_TYPE_RELAY && !pair->local_connected) {
  agent_send_bind_request(agent,&agent->send_msg,pair);return;
 }
 if(agent->selected_pair==pair && agent->mode==AGENT_MODE_CONTROLLING) {
  pair->local_connected=true;
  if(pair->state!=ICE_CANDIDATE_STATE_SUCCEEDED) {
   LOGI("%d Candidate responded",agent->mode);pair->peer_connected=true;
   if(agent->ice_use_lite_mode)LOGI("ICE lite connected");
   pair->state=ICE_CANDIDATE_STATE_SUCCEEDED;
  }
 }
 if(!agent->selected_pair) {
  if(pair->type==ICE_CANDIDATE_TYPE_RELAY && agent_relay_should_defer(agent))return;
  agent->selected_pair=pair;
  char ip_str[64];
  LOGI("%d Select pair%s",agent->mode,esp_peer_addr_to_str(&agent->selected_pair->remote->addr,ip_str));
  if(agent->mode==AGENT_MODE_CONTROLLING)pair->local_connected=false;
  if(!pair->local_connected) {
   if(pair->type==ICE_CANDIDATE_TYPE_RELAY)agent_send_indication(agent,pair);
   else agent_send_bind_request(agent,&agent->send_msg,pair);
  }
 }else if(agent->mode==AGENT_MODE_CONTROLLING && agent->selected_pair!=pair && pair->type!=ICE_CANDIDATE_TYPE_RELAY &&
    agent->selected_pair->type!=ICE_CANDIDATE_TYPE_HOST && pair->priority>agent->selected_pair->priority && pair->local_connected && pair->peer_connected) {
  char ip_str[64];
  LOGI("%d Override pair (type %d -> %d)%s",agent->mode,agent->selected_pair->type,pair->type,esp_peer_addr_to_str(&pair->remote->addr,ip_str));
  if(agent->selected_pair->type==ICE_CANDIDATE_TYPE_RELAY)agent->use_stun_relay=false;
  agent->selected_pair=pair;pair->local_connected=false;
  agent_send_bind_request(agent,&agent->send_msg,pair);
 }
 if(pair->type==ICE_CANDIDATE_TYPE_RELAY) {
  if(pair==agent->selected_pair && !pair->channel_binding)agent_send_channel_bind(agent,pair,false);
 }
}

PRIVATE int agent_recv_tcp_packet(agent_t *agent,esp_peer_addr_t *addr,uint8_t *buf,int len,bool no_wait) {
 if (agent->tcp_turn_connected && agent_turn_stream_is_tls(agent)) {
  if (agent->tls_turn) return tls_connections_recv_raw_from(agent->tls_turn,addr,buf,len,no_wait);
  return 0;
 }
 if (!agent->tcp) return 0;
 if (agent->tcp_turn_connected) return tcp_connections_recv_raw_from(agent->tcp,addr,buf,len,no_wait);
 return tcp_connections_recv_from(agent->tcp,addr,buf,len,no_wait);
}

PRIVATE void dump_recv_data(esp_peer_addr_t *addr,uint8_t *data,int len) {
 if (len>16)len=16;
 if (addr) {char ip_str[64];printf("Get Data from %s\n",esp_peer_addr_to_str(addr,ip_str));}
 for(int i=0;i<len;i++) {
  if (i && i%16==0)printf("\n");
  printf("%02x ",data[i]);
 }
 printf("\n");
}

PRIVATE bool agent_same_addr(const esp_peer_addr_t *a,const esp_peer_addr_t *b) {
 if (a->family!=b->family || a->port!=b->port) return false;
 if (a->family==2) return memcmp(a->ipv4,b->ipv4,sizeof(a->ipv4))==0;
 if (a->family==10) return memcmp(a->ipv6,b->ipv6,sizeof(a->ipv6))==0;
 return false;
}

PRIVATE bool agent_is_turn_server_addr(agent_t *agent,esp_peer_addr_t *addr) {
 bool same=agent_same_addr(addr,&agent->stun_addr);
 if(same)return same;
 return agent->tcp_turn_connected && agent_same_addr(addr,&agent->tcp_turn_addr);
}

void *jetkvm_ice_find_pair(void *handle,const esp_peer_addr_t *addr) {
 agent_t *agent=handle;
 for (int i=0;i<agent->candidate_pairs_num;i++) {
  ice_candidate_pair_t *pair=agent->sort_pairs[i];
  if (agent_same_addr(addr,&pair->remote->addr)) return pair;
 }
 if (agent->tcp) {
  for (int i=0;i<agent->candidate_pairs_num;i++) {
   ice_candidate_pair_t *pair=agent->sort_pairs[i];
   if (pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP && pair->remote->addr.family==addr->family) return pair;
  }
 }
 return NULL;
}

PRIVATE void agent_process_stun_request(agent_t *agent, esp_peer_addr_t *remote, stun_msg_t *stun_msg)
{
    char ip_str[128];
    switch (stun_msg->stunmethod) {
        case STUN_METHOD_BINDING:
            if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->local_upwd) == 0) {
                ice_candidate_pair_t *pair = NULL;
                if (agent_is_turn_server_addr(agent, remote)) {
                    if (agent->selected_pair && agent->selected_pair->type == ICE_CANDIDATE_TYPE_RELAY) {
                        pair = agent->selected_pair;
                        pair->peer_connected = true;
                        agent_response_indication(agent, pair, stun_msg);
                        break;
                    }
                } else {
                    /* Only authenticated direct Binding Requests can learn a
                     * peer-reflexive candidate through the application hook. */
                    pair = jetkvm_ice_authenticated_pair(agent, remote, stun_msg->buf,
                                                         (size_t)stun_msg->size);
                }
                if (!pair) {
                    LOGE("%d Not found for bind remote %s", agent->mode, esp_peer_addr_to_str(remote, ip_str));
                    break;
                }
                stun_msg_t msg;
                bool use_candidate = stun_msg->use_candidate;
                if (agent->stun_state != ICE_CANDIDATE_STATE_SUCCEEDED) {
                    LOGI("%d %d PeerBinding recv local%d:%s remote%d:%s", agent->mode,
                         pair - agent->candidate_pairs, pair->local_connected,
                         esp_peer_addr_to_str(&pair->local->addr, ip_str), pair->peer_connected,
                         esp_peer_addr_to_str(&pair->remote->addr, ip_str + 64));
                }
                if (use_candidate || agent->mode != AGENT_MODE_CONTROLLED) {
                    pair->peer_connected = true;
                }
                if (pair->local_connected) {
                    agent->last_peer_binding_time = get_cur_time();
                    agent->binding_not_response = 0;
                }
                agent_build_binding_response(agent, pair, stun_msg, &msg);
                if (pair->local->transport == ICE_CANDIDATE_TRANSPORT_TCP) {
                    agent_tcp_send_packet(agent, remote, msg.buf, msg.size, 2);
                } else {
                    udp_socket_sendto(&agent->stun_sock, remote, msg.buf, msg.size);
                }
                if (agent->mode == AGENT_MODE_CONTROLLED) {
                    agent_select_pair_try_select(agent, pair, use_candidate);
                }
            }
            break;
        case STUN_METHOD_DATA: {
            ice_candidate_pair_t *pair = jetkvm_ice_find_pair(agent, &stun_msg->relayed_addr);
            if (!pair) {
                LOGE("%d Not found for bind remote22 %s", agent->mode, esp_peer_addr_to_str(remote, ip_str));
                break;
            }
            if (!stun_msg->data) {
                break;
            }
            memmove(stun_msg->buf, stun_msg->data, stun_msg->data_size);
            stun_msg->use_candidate = false;
            stun_parse_msg_buf(stun_msg);
            if (stun_msg->stunmethod != STUN_METHOD_BINDING) {
                break;
            }
            if (stun_msg->stunclass == STUN_CLASS_REQUEST) {
                LOGI("%d PeerIndication recv local:%s remote:%s", agent->mode,
                     esp_peer_addr_to_str(&pair->local->addr, ip_str),
                     esp_peer_addr_to_str(&pair->remote->addr, ip_str + 64));
                bool use_candidate = stun_msg->use_candidate;
                if (use_candidate || agent->mode != AGENT_MODE_CONTROLLED) {
                    pair->peer_connected = true;
                }
                agent_response_indication(agent, pair, stun_msg);
                if (agent->mode == AGENT_MODE_CONTROLLED) {
                    agent_select_pair_try_select(agent, pair, use_candidate);
                }
            } else if (stun_msg->stunclass == STUN_CLASS_RESPONSE) {
                LOGI("%d LocalIndication resp local:%s remote:%s", agent->mode,
                     esp_peer_addr_to_str(&pair->local->addr, ip_str),
                     esp_peer_addr_to_str(&pair->remote->addr, ip_str + 64));
                pair->local_connected = true;
                if (agent->mode == AGENT_MODE_CONTROLLING) {
                    agent_select_pair_try_select(agent, pair, true);
                } else if (agent->mode == AGENT_MODE_CONTROLLED) {
                    agent_select_pair_try_select(agent, pair, pair->peer_connected);
                }
            }
            if (agent->selected_pair == pair) {
                agent->binding_not_response = 0;
            }
            break;
        }
        default:
            break;
    }
}

PRIVATE ice_candidate_pair_t *find_pair_by_transaction(agent_t *agent,uint32_t *id) {
 for (int i=0;i<agent->candidate_pairs_num;i++) {
  ice_candidate_pair_t *pair=agent->sort_pairs[i];
  if (memcmp(pair->binding_transaction,id,sizeof(pair->binding_transaction))==0) return pair;
 }
 return NULL;
}

PRIVATE ice_candidate_pair_t *find_pair_by_channel_transaction(agent_t *agent,uint32_t *id) {
 for (int i=0;i<agent->candidate_pairs_num;i++) {
  ice_candidate_pair_t *pair=agent->sort_pairs[i];
  if (memcmp(pair->channel_transaction,id,sizeof(pair->channel_transaction))==0) return pair;
 }
 return NULL;
}

PRIVATE void agent_process_stun_response(agent_t *agent, esp_peer_addr_t *remote, stun_msg_t *stun_msg)
{
    switch (stun_msg->stunmethod) {
        case STUN_METHOD_BINDING:
            if (agent->remote_upwd && stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->remote_upwd) == 0) {
                stun_header_t *header = (stun_header_t *)stun_msg->buf;
                ice_candidate_pair_t *cur = find_pair_by_transaction(agent, header->transaction_id);
                if (!cur) {
                    if (agent_is_turn_server_addr(agent, remote) && agent->selected_pair &&
                        agent->selected_pair->type == ICE_CANDIDATE_TYPE_RELAY) {
                        cur = agent->selected_pair;
                    } else {
                        cur = jetkvm_ice_find_pair(agent, remote);
                    }
                    if (!cur) {
                        break;
                    }
                }
                if (!cur->local_connected) {
                    LOGI("%d %d LocalBinding resp %08x%08x%08x local%d", agent->mode,
                         cur - agent->candidate_pairs, (unsigned int)header->transaction_id[0],
                         (unsigned int)header->transaction_id[1], (unsigned int)header->transaction_id[2], true);
                }
                cur->local_connected = true;
                if (agent->mode == AGENT_MODE_CONTROLLING) {
                    agent_select_pair_try_select(agent, cur, true);
                } else if (agent->mode == AGENT_MODE_CONTROLLED) {
                    agent_select_pair_try_select(agent, cur, cur->peer_connected);
                }
                agent->last_peer_binding_time = get_cur_time();
                if (agent->selected_pair == cur) {
                    agent->binding_not_response = 0;
                }
            }
            break;
        case STUN_METHOD_CHANNEL_BIND: {
            stun_header_t *header = (stun_header_t *)stun_msg->buf;
            LOGI("%d Receive Channel bind response id:%08x%08x%08x", agent->mode,
                 (unsigned int)header->transaction_id[0], (unsigned int)header->transaction_id[1], header->transaction_id[2]);
            ice_candidate_pair_t *cur = find_pair_by_channel_transaction(agent, header->transaction_id);
            if (!cur) {
                LOGE("Not find pair for Channel bind response");
                break;
            }
            LOGI("%d Channel bind success, connection DONE", agent->mode);
            agent->use_stun_relay = true;
            agent->stun_state = ICE_CANDIDATE_STATE_SUCCEEDED;
            agent->last_peer_binding_time = get_cur_time();
            cur->state = ICE_CANDIDATE_STATE_SUCCEEDED;
            break;
        }
        case STUN_METHOD_CREATE_PERMISSION:
            if (stun_msg_is_valid_response(stun_msg->buf, stun_msg->size, STUN_CREDENTIAL_LONG_TERM,
                                           agent->stun_user, agent->realm, agent->stun_psw) == 0) {
                agent->last_permission_time = get_cur_time();
                LOGI("%d receive permission response", agent->mode);
            }
            break;
        case STUN_METHOD_REFRESH:
            if (stun_msg_is_valid_response(stun_msg->buf, stun_msg->size, STUN_CREDENTIAL_LONG_TERM,
                                           agent->stun_user, agent->realm, agent->stun_psw) == 0) {
                agent->last_fresh_time = get_cur_time();
                LOGI("%d receive refresh response", agent->mode);
            }
            break;
        default:
            break;
    }
}

PRIVATE int agent_recv_one_packet(agent_t *agent,uint8_t *buf,int len,bool no_wait,bool *is_stun) {
 int ret;
 esp_peer_addr_t dst_addr;
 if (is_stun) *is_stun=false;
 if (agent->stun_sock.fd<=0) return -3;
 pthread_mutex_lock(&agent->recv_mutex);
 bool prefer_tcp=agent->selected_pair && (agent->selected_pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP ||
  (agent->selected_pair->type==ICE_CANDIDATE_TYPE_RELAY && agent->tcp_turn_connected));
 bool has_tcp_stream=agent->tcp || (agent->tcp_turn_connected && agent_turn_stream_is_tls(agent) && agent->tls_turn);
 if (has_tcp_stream && (prefer_tcp || no_wait)) {
  ret=agent_recv_tcp_packet(agent,&dst_addr,buf,len,no_wait);
  if (ret>0) goto process_packet;
  if (prefer_tcp) {pthread_mutex_unlock(&agent->recv_mutex);return ret;}
 } else if (agent->tcp) {
  ret=agent_recv_tcp_packet(agent,&dst_addr,buf,len,true);
  if (ret>0) goto process_packet;
 }
 if (agent->use_stun_relay || agent->mode==AGENT_MODE_CONTROLLED) {
  ret=udp_socket_recvfrom_nowait(&agent->stun_sock,&dst_addr,buf,len,no_wait);
  if (ret<=0) {pthread_mutex_unlock(&agent->recv_mutex);return ret;}
  if (is_channel_data(agent,buf,ret)) {
   ret-=4;
   uint16_t channel_data_size=__builtin_bswap16(*(uint16_t *)(buf+2));
   if (channel_data_size<ret) ret=channel_data_size;
   memmove(buf,buf+4,ret);
   if (agent->use_stun_relay && agent->stun_state==ICE_CANDIDATE_STATE_SUCCEEDED) {
    agent->last_peer_binding_time=get_cur_time();agent->binding_not_response=0;
   }
  }
 } else {
  ret=udp_socket_recvfrom_nowait(&agent->stun_sock,&dst_addr,buf,len,no_wait);
  if (ret<=0) {pthread_mutex_unlock(&agent->recv_mutex);return ret;}
 }
 if (allow_dump_recv) dump_recv_data(&dst_addr,buf,ret);
process_packet:
 if (is_channel_data(agent,buf,ret)) {
  ret-=4;
  uint16_t channel_data_size=__builtin_bswap16(*(uint16_t *)(buf+2));
  if (channel_data_size<ret) ret=channel_data_size;
  memmove(buf,buf+4,ret);
  if (agent->use_stun_relay && agent->stun_state==ICE_CANDIDATE_STATE_SUCCEEDED) {
   agent->last_peer_binding_time=get_cur_time();agent->binding_not_response=0;
  }
 }
 if (ret>0 && stun_probe(buf,ret)==0) {
  stun_msg_t stun_msg;
  memcpy(stun_msg.buf,buf,ret);stun_msg.size=ret;
  stun_parse_msg_buf(&stun_msg);
  if (stun_msg.stunclass==STUN_CLASS_INDICATION && stun_msg.stunmethod==STUN_METHOD_DATA && stun_probe(stun_msg.data,stun_msg.data_size)) {
   memcpy(buf,stun_msg.data,stun_msg.data_size);
   if (agent->use_stun_relay && agent->stun_state==ICE_CANDIDATE_STATE_SUCCEEDED) {
    agent->last_peer_binding_time=get_cur_time();agent->binding_not_response=0;
   }
   if (allow_dump_recv) dump_recv_data(NULL,buf,ret);
   pthread_mutex_unlock(&agent->recv_mutex);return stun_msg.data_size;
  }
  if (is_stun) *is_stun=true;
  switch(stun_msg.stunclass) {
  case STUN_CLASS_REQUEST: case STUN_CLASS_INDICATION:
   agent_process_stun_request(agent,&dst_addr,&stun_msg);break;
  case STUN_CLASS_RESPONSE:
   agent_process_stun_response(agent,&dst_addr,&stun_msg);break;
  case STUN_CLASS_ERROR:
   if (stun_msg.stunmethod==STUN_METHOD_REFRESH && (stun_msg.err_code==401 || stun_msg.err_code==438)) {
    if (agent_update_credential(agent,&stun_msg)==0) agent_send_fresh(agent,300);
    break;
   }
   if (stun_msg.stunmethod==STUN_METHOD_CREATE_PERMISSION && (stun_msg.err_code==401 || stun_msg.err_code==438)) {
    if (agent_update_credential(agent,&stun_msg)==0 && agent->use_stun_relay && agent->selected_pair)
     agent_send_permission(agent,agent->selected_pair->remote,true);
    break;
   }
   {
    ice_candidate_pair_t *pair=jetkvm_ice_find_pair(agent,&dst_addr);
    if (pair) pair->state=ICE_CANDIDATE_STATE_WAITING;
   }
   break;
  default:break;
  }
 }
 pthread_mutex_unlock(&agent->recv_mutex);return ret;
}

PRIVATE int agent_recv_all(agent_t *agent,uint8_t *buf,int len) {
 int ret;bool no_wait=false;
 do {ret=agent_recv_one_packet(agent,buf,len,no_wait,NULL);no_wait=true;}while(ret>0);
 return ret;
}

PRIVATE int agent_set_remote_ice_cred(char **dst,const char *src,size_t max_len) {
 if (!dst || !src) return -1;
 size_t len=strlen(src);
 if (len>max_len) len=max_len;
 char *buf=media_lib_malloc(len+1);
 if (!buf) return -1;
 memcpy(buf,src,len);buf[len]=0;
 if (*dst) media_lib_free(*dst);
 *dst=buf;return 0;
}

void jetkvm_ice_add_remote(void *handle,char *candidate,char *end) {
 agent_t *agent=handle;
 if (agent->remote_candidates_count>=agent->max_candidates) {LOGE("%d Remote candidate over limited %d",agent->mode,agent->remote_candidates_count);return;}
 ice_candidate_t *cur=&agent->remote_candidates[agent->remote_candidates_count];
 memset(cur,0,sizeof(*cur));
 if (ice_candidate_from_description(cur,candidate,end)!=0)return;
 if (force_tcp_only && cur->transport!=ICE_CANDIDATE_TRANSPORT_TCP)return;
 for(int i=0;i<agent->remote_candidates_count;i++) {
  if (cur->type==agent->remote_candidates[i].type && cur->transport==agent->remote_candidates[i].transport &&
      agent_same_addr(&cur->addr,&agent->remote_candidates[i].addr)) return;
 }
 char ip_str[64];
 LOGI("%d Add remote type:%d %s",agent->mode,cur->type,esp_peer_addr_to_str(&cur->addr,ip_str));
 if (cur->type!=ICE_CANDIDATE_TYPE_RELAY && agent->stun_state!=ICE_CANDIDATE_STATE_SUCCEEDED)
  agent->stun_state=ICE_CANDIDATE_STATE_FROZEN;
 agent->remote_candidates_count++;
}

PRIVATE bool agent_have_ongoing_nominate(agent_t *agent) {
 time_t t=time(NULL);int on_going=0;
 for (int i=0;i<agent->candidate_pairs_num;i++) {
  ice_candidate_pair_t *pair=agent->sort_pairs[i];
  if (agent_nominate_timeout(pair,t)) {pair->state=ICE_CANDIDATE_STATE_FAILED;continue;}
  on_going++;
 }
 return on_going>0;
}

PRIVATE int agent_check_nominates(agent_t *agent) {
 int ret=-1;
 for(int i=0;i<agent->candidate_pairs_num;i++) {
  ice_candidate_pair_t *pair=agent->sort_pairs[i];
  if(pair->state==ICE_CANDIDATE_STATE_FAILED)continue;
  uint32_t t=get_cur_time();
  if(!(t+2000<pair->last_bind_time) && pair->last_bind_time+400>=t)continue;
  if(pair->state==ICE_CANDIDATE_STATE_FROZEN){pair->state=ICE_CANDIDATE_STATE_INPROGRESS;pair->nominated_time=time(NULL);}
  if(agent->selected_pair && agent->selected_pair!=pair) {
   if(agent->mode!=AGENT_MODE_CONTROLLING || pair->type==ICE_CANDIDATE_TYPE_RELAY || agent->selected_pair->type==ICE_CANDIDATE_TYPE_HOST ||
     pair->priority<=agent->selected_pair->priority)continue;
  }
  pair->last_bind_time=t;
  if(pair->type==ICE_CANDIDATE_TYPE_RELAY) {
   if(pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP) {
    if(agent_relay_should_defer(agent))continue;
    agent->selected_pair=pair;pair->local_connected=true;pair->peer_connected=true;pair->state=ICE_CANDIDATE_STATE_SUCCEEDED;
    agent->use_stun_relay=true;agent->stun_state=ICE_CANDIDATE_STATE_SUCCEEDED;agent->last_peer_binding_time=t;ret=0;
    continue;
   }
   ret=agent_send_indication(agent,pair);
  }else {
   if(force_relay || agent->relay_only)continue;
   if(pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP && !tcp_connections_is_connected(agent->tcp,&pair->remote->addr)) {
    tcp_connections_connect(agent->tcp,&pair->remote->addr);continue;
   }
   ret=agent_send_bind_request(agent,&agent->send_msg,pair);
  }
  if(ret<0){LOGE("Send bind request failed");break;}
 }
 return ret;
}

agent_handle_t agent_init(agent_cfg_t *cfg) {
 agent_t *agent=media_lib_calloc(1,sizeof(*agent));
 if(!agent)return NULL;
 bool inst_lock_create=false,recv_lock_create=false;
 agent->max_candidates=cfg->max_candidates?cfg->max_candidates:10;
 agent->tcp_support=cfg->tcp_support;
 agent->max_tcp_connections=cfg->max_tcp_connections?cfg->max_tcp_connections:4;
 int max_paired=agent->max_candidates*2;
 agent->max_paired_candidates=max_paired<=64?(uint8_t)max_paired:64;
 agent->ice_use_lite_mode=cfg->ice_use_lite_mode;
 agent->turn_skip_cert_verify=cfg->turn_skip_cert_verify;
 agent->alive_binding_retries=cfg->alive_binding_retries?cfg->alive_binding_retries:5;
 agent->local_candidates=media_lib_calloc(1,sizeof(ice_candidate_t)*agent->max_candidates);
 if(!agent->local_candidates)goto fail;
 agent->remote_candidates=media_lib_calloc(1,sizeof(ice_candidate_t)*agent->max_candidates);
 if(!agent->remote_candidates)goto fail;
 agent->candidate_pairs=media_lib_calloc(1,sizeof(ice_candidate_pair_t)*agent->max_paired_candidates);
 if(!agent->candidate_pairs)goto fail;
 agent->sort_pairs=media_lib_calloc(1,sizeof(ice_candidate_pair_t*)*agent->max_paired_candidates);
 if(!agent->sort_pairs)goto fail;
 if(pthread_mutex_init(&agent->mutex,NULL)!=0)goto fail;
 inst_lock_create=true;
 if(pthread_mutex_init(&agent->recv_mutex,NULL)!=0)goto fail;
 recv_lock_create=true;
 agent->mode=cfg->mode;agent->ipv6_support=cfg->ipv6_support;
 if(agent->tcp_support) {
  tcp_connections_cfg_t tcp_cfg={.ipv6_support=agent->ipv6_support,.max_connections=agent->max_tcp_connections,
   .connect_timeout_ms=30000,.send_timeout_ms=1000,.recv_timeout_ms=agent->recv_timeout?agent->recv_timeout:150};
  agent->tcp=tcp_connections_open(&tcp_cfg);
  if(!agent->tcp)goto fail;
 }
 return agent;
fail:
 LOGE("Not enough memory for agent");
 if(agent->tcp) {tcp_connections_close(agent->tcp);agent->tcp=NULL;}
 if(agent->local_candidates)media_lib_free(agent->local_candidates);
 if(agent->remote_candidates)media_lib_free(agent->remote_candidates);
 if(agent->candidate_pairs)media_lib_free(agent->candidate_pairs);
 if(agent->sort_pairs)media_lib_free(agent->sort_pairs);
 if(recv_lock_create)pthread_mutex_destroy(&agent->recv_mutex);
 if(inst_lock_create)pthread_mutex_destroy(&agent->mutex);
 media_lib_free(agent);return NULL;
}

void agent_set_recv_timeout(agent_handle_t agent,uint16_t timeout) {
 agent->recv_timeout=timeout;
 if (agent->tcp) tcp_connections_set_recv_timeout(agent->tcp,timeout);
 if (agent->tls_turn) tls_connections_set_recv_timeout(agent->tls_turn,timeout);
}

void agent_change_mode(agent_handle_t agent,agent_mode_t mode) {
 LOGI("Start agent as %s",mode==AGENT_MODE_CONTROLLING?"Controlling":"Controlled");
 agent->local_candidates_count=0;agent->candidate_pairs_num=0;agent->mode=mode;
}

int agent_add_tcp_stun_server(agent_handle_t agent,const char *urls,const char *username,const char *credential) {
 if(!agent || !urls)return -1;
 if(!agent->tcp_support){LOGW("Skip TCP/TLS TURN server when TCP support is disabled");return -4;}
 int scheme_len;bool use_tls;
 if(strncmp(urls,"turns:",6)==0){scheme_len=6;use_tls=true;}
 else if(strncmp(urls,"turn:",5)==0){scheme_len=5;use_tls=false;}
 else {LOGW("Expected turn: or turns: URL with transport=tcp: %s",urls);return -4;}
 for(int i=0;i<agent->local_candidates_count;i++) {
  if(agent->local_candidates[i].type==ICE_CANDIDATE_TYPE_RELAY && agent->local_candidates[i].transport==ICE_CANDIDATE_TRANSPORT_TCP) {
   LOGI("Skip TCP TURN server %s for only support one",urls);return 0;
  }
 }
 char *port=strchr(urls+scheme_len,':');if(!port)return -8;
 char hostname[64];memset(hostname,0,sizeof(hostname));
 snprintf(hostname,port-urls-scheme_len+1,"%s",urls+scheme_len);
 esp_peer_addr_t resolved_addr={0};
 if(!addr_ipv4_validate(hostname,strlen(hostname),&resolved_addr)) {
  if(ports_resolve_addr(hostname,&resolved_addr)!=0){LOGE("Fail to resolve TCP TURN server address");return -6;}
 }
 resolved_addr.port=atoi(port+1);if(!resolved_addr.port)return -8;
 return agent_gather_tcp_turn_candidate(agent,&resolved_addr,username,credential,use_tls,hostname);
}

void agent_gather_candidate(agent_handle_t agent,const char *urls,const char *username,const char *credential) {
 char *port;char hostname[64];esp_peer_addr_t resolved_addr;
 agent_create_bind_addr(agent,NULL,NULL,NULL);
 if(!agent->relay_only && !agent->local_candidates_count) {
  esp_peer_addr_t host_addr;
  if(!force_tcp_only && !(force_ipv6_only && agent->ipv6_support)) {
   udp_get_local_address(&agent->stun_sock,false,&host_addr);
   if(agent_create_host_addr(agent,host_addr.port,&host_addr)==0)agent_add_local_candidate(agent,ICE_CANDIDATE_TYPE_HOST,&host_addr);
  }
  if(!force_tcp_only && agent->ipv6_support) {
   udp_get_local_address(&agent->stun_sock,true,&host_addr);
   if(agent_create_host_addr(agent,host_addr.port,&host_addr)==0)agent_add_local_candidate(agent,ICE_CANDIDATE_TYPE_HOST,&host_addr);
   else LOGW("No IPV6 address found");
  }
  agent_gather_tcp_host_candidate(agent);
 }
 if(urls) {
 int scheme_len;
 if(strncmp(urls,"turns:",6)==0)scheme_len=6;
 else if(strncmp(urls,"stun:",5)==0)scheme_len=5;
 else if(strncmp(urls,"turn:",5)==0)scheme_len=5;
 else return;
 port=strchr(urls+scheme_len,':');if(!port)return;
 if(strstr(urls,"transport=tcp")){agent_add_tcp_stun_server(agent,urls,username,credential);}
 else {
 for(int i=0;i<agent->local_candidates_count;i++) {
  if(agent->local_candidates[i].type==ICE_CANDIDATE_TYPE_RELAY && username) {LOGI("Skip TURN server %s for only support one",urls);return;}
 }
 memset(hostname,0,sizeof(hostname));
 snprintf(hostname,port-urls-scheme_len+1,"%s",urls+scheme_len);
 if(!addr_ipv4_validate(hostname,strlen(hostname),&resolved_addr)) {
  if(ports_resolve_addr(hostname,&resolved_addr)!=0){LOGE("Fail to resolve server address");return;}
 }
 resolved_addr.port=atoi(port+1);if(!resolved_addr.port)return;
 if(strncmp(urls,"stun:",5)==0 || strncmp(urls,"turn:",5)==0 || strncmp(urls,"turns:",6)==0)
  agent_create_bind_addr(agent,&resolved_addr,username,credential);
 }
 }
}

int agent_get_local_description(agent_handle_t agent,char *description,int length) {
 description[0]=0;
 if (!agent->local_ufrag[0]) {utils_random_string(agent->local_ufrag,4);utils_random_string(agent->local_upwd,24);}
 snprintf(description,length,"a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n",agent->local_ufrag,agent->local_upwd);
 pthread_mutex_lock(&agent->mutex);
 for(int i=0;i<agent->local_candidates_count;i++) {
  description+=strlen(description);length-=strlen(description);
  ice_candidate_to_description(&agent->local_candidates[i],description,length);
 }
 jetkvm_ice_pair_candidates(agent);
 pthread_mutex_unlock(&agent->mutex);return agent->local_candidates_count?0:-5;
}

int agent_send(agent_handle_t agent,const uint8_t *buf,int len) {return agent_send_with_prio(agent,buf,len,1);}

int agent_send_high_priority(agent_handle_t agent,const uint8_t *buf,int len) {return agent_send_with_prio(agent,buf,len,2);}

int agent_send_to_port(agent_handle_t agent,int port,const uint8_t *buf,int len) {
 if (!agent) return -1;
 if (agent->selected_pair && agent->selected_pair->type==ICE_CANDIDATE_TYPE_RELAY && agent->tcp_turn_connected)
  return agent_send_turn_data(agent,agent->selected_pair->remote,buf,len,1);
 if (agent->use_stun_relay) {
  if (len+4>=sizeof(agent->send_msg.buf)) {LOGE("Drop for size %d over receive buf size %d\n",len,sizeof(agent->send_msg.buf));return -7;}
  uint8_t *send_buf=agent->send_msg.buf;
  *(uint16_t*)send_buf=agent->stun_channel_num;
  *(uint16_t*)(send_buf+2)=__builtin_bswap16(len);
  memcpy(send_buf+4,buf,len);
  esp_peer_addr_t target=agent->stun_addr;target.port=port;
  int ret=udp_socket_sendto(&agent->stun_sock,&target,send_buf,len+4);
  return ret>4?ret-4:ret;
 }
 if (!agent->selected_pair || !agent->stun_sock.fd) return -3;
 esp_peer_addr_t target=agent->selected_pair->remote->addr;target.port=port;
 if (agent->selected_pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP)
  return agent_tcp_send_packet(agent,&target,buf,len,1);
 return udp_socket_sendto(&agent->stun_sock,&target,buf,len);
}

int agent_send_bind_directly(agent_t *agent) {
 if (agent->stun_state==ICE_CANDIDATE_STATE_SUCCEEDED && agent->selected_pair)agent_send_indication(agent,agent->selected_pair);
 return 0;
}

void agent_allow_recv_dump(bool enable) {allow_dump_recv=enable;}

int agent_recv(agent_handle_t agent,uint8_t *buf,int len,bool no_wait) {
 if (!agent) return -1;
 int ret;bool is_stun=false;
 do {ret=agent_recv_one_packet(agent,buf,len,no_wait,&is_stun);}while(!no_wait && ret>0 && is_stun);
 return ret;
}

int agent_set_remote_description(agent_handle_t agent,char *description) {
 char *line_start=description,*line_end=NULL;
 pthread_mutex_lock(&agent->mutex);
 if (memcmp(description,"candidate:",10)==0) {
  jetkvm_ice_add_remote(agent,description+10,description+strlen(description));
 }else {
  bool got_ufrag=false,got_upwd=false;
  while(true) {
   int step=2;
   line_end=strstr(line_start,"\r\n");
   if(!line_end) {line_end=strchr(line_start,'\n');step=1;}
   if(!line_end)break;
   if (memcmp(line_start,"a=ice-ufrag:",12)==0) {
    char saved=*line_end;*line_end=0;
    if (!got_ufrag && agent_set_remote_ice_cred(&agent->remote_ufrag,line_start+12,256)==0)got_ufrag=true;
    *line_end=saved;
   }else if(memcmp(line_start,"a=ice-pwd:",10)==0) {
    char saved=*line_end;*line_end=0;
    if (!got_upwd && agent_set_remote_ice_cred(&agent->remote_upwd,line_start+10,256)==0)got_upwd=true;
    *line_end=saved;
   }else if(memcmp(line_start,"a=candidate:",12)==0)jetkvm_ice_add_remote(agent,line_start+12,line_end);
   line_start=line_end+step;
  }
 }
 jetkvm_ice_pair_candidates(agent);
 int remote_count=agent->remote_candidates_count;
 pthread_mutex_unlock(&agent->mutex);return remote_count;
}

int agent_get_remote_ice(agent_handle_t agent,const char **ufrag,const char **pwd) {
 if (!agent || !ufrag || !pwd) return -1;
 pthread_mutex_lock(&agent->mutex);
 *ufrag=agent->remote_ufrag&&agent->remote_ufrag[0]?agent->remote_ufrag:NULL;
 *pwd=agent->remote_upwd&&agent->remote_upwd[0]?agent->remote_upwd:NULL;
 pthread_mutex_unlock(&agent->mutex);return 0;
}

int agent_select_candidate_pair(agent_handle_t agent) {
 pthread_mutex_lock(&agent->mutex);
 if (!agent->local_candidates_count) {pthread_mutex_unlock(&agent->mutex);return -5;}
 agent_candidate_prepare(agent);
 bool checking=agent_have_ongoing_nominate(agent);
 pthread_mutex_unlock(&agent->mutex);return checking?0:-6;
}

bool agent_connectivity_check(agent_handle_t agent) {
 pthread_mutex_lock(&agent->mutex);
 if(!agent->local_candidates_count){pthread_mutex_unlock(&agent->mutex);return false;}
 udp_blocking_timeout(&agent->stun_sock,150);
 bool connected=agent_is_connected(agent);
 if(!connected) {
  agent_check_nominates(agent);
  bool no_wait=false;
  while(!connected) {
   int ret=agent_recv_one_packet(agent,agent->recv_buf,sizeof(agent->recv_buf),no_wait,NULL);
   if(ret<=0)break;
   connected=agent_is_connected(agent);no_wait=true;
  }
 }
 if(connected) {
  udp_blocking_timeout(&agent->stun_sock,agent->recv_timeout?agent->recv_timeout:150);
  ice_candidate_pair_t *pair=agent->selected_pair;char ip_str[64];
  LOGI("%d Connection OK %s",agent->mode,esp_peer_addr_to_str(&pair->remote->addr,ip_str));
  agent->stun_state=ICE_CANDIDATE_STATE_SUCCEEDED;agent->last_peer_binding_time=get_cur_time();
 }
 pthread_mutex_unlock(&agent->mutex);return connected;
}

int agent_get_paired(agent_handle_t agent,esp_peer_addr_t *addr) {
 if (!agent) return -1;
 pthread_mutex_lock(&agent->mutex);
 bool connected=agent_is_connected(agent);
 if (connected && agent->selected_pair) memcpy(addr,&agent->selected_pair->remote->addr,sizeof(*addr));
 pthread_mutex_unlock(&agent->mutex);
 return connected?0:-3;
}

int agent_get_ice_selected_summary(agent_handle_t agent,agent_ice_selected_summary_t *out) {
 if (!agent || !out) return -1;
 memset(out,0,sizeof(*out));
 pthread_mutex_lock(&agent->mutex);
 if (!agent->selected_pair || !agent->selected_pair->local) {
  pthread_mutex_unlock(&agent->mutex);return -3;
 }
 out->valid=true;
 out->local_candidate_type=(uint8_t)agent->selected_pair->local->type;
 out->local_transport=(uint8_t)agent->selected_pair->local->transport;
 out->pair_type=(uint8_t)agent->selected_pair->type;
 pthread_mutex_unlock(&agent->mutex);return 0;
}

bool agent_is_tcp_connected(agent_handle_t agent) {
 if (!agent) return false;
 pthread_mutex_lock(&agent->mutex);
 bool tcp_connected=agent_is_connected(agent);
 if (tcp_connected) {
  if (agent->selected_pair) tcp_connected=agent->selected_pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP;
  else tcp_connected=false;
 }
 pthread_mutex_unlock(&agent->mutex);return tcp_connected;
}

bool agent_is_relay_connection(agent_handle_t agent) {
 if (!agent) return false;
 pthread_mutex_lock(&agent->mutex);
 bool relay=agent->selected_pair && agent->selected_pair->type==ICE_CANDIDATE_TYPE_RELAY;
 pthread_mutex_unlock(&agent->mutex);return relay;
}

void agent_force_ipv6_only(bool ipv6_only) {force_ipv6_only=ipv6_only;}

void agent_force_relay(bool relay) {force_relay=relay;}

void agent_set_tcp_only(bool tcp_only) {force_tcp_only=tcp_only;}

int agent_set_ice_policy(agent_handle_t agent,bool relay_only) {agent->relay_only=relay_only;return 0;}

int agent_check_alive(agent_handle_t agent) {
 if (!agent) return -1;
 pthread_mutex_lock(&agent->mutex);
 if (!agent->local_candidates_count) {pthread_mutex_unlock(&agent->mutex);return 0;}
 bool selected_tcp_relay=agent->selected_pair && agent->selected_pair->local->transport==ICE_CANDIDATE_TRANSPORT_TCP && agent->selected_pair->type==ICE_CANDIDATE_TYPE_RELAY;
 if (agent->local_candidates_count>1 && !selected_tcp_relay) {
  uint32_t cur=get_cur_time();
  stun_msg_t *send_msg=&agent->send_msg;
  if (cur<agent->last_stun_binding_time || cur>=agent->last_stun_binding_time+10000) {
   stun_msg_create(send_msg,STUN_METHOD_BINDING,false);
   stun_msg_finish(send_msg,STUN_CREDENTIAL_NONE,NULL,0);
   udp_socket_sendto(&agent->stun_sock,&agent->stun_addr,send_msg->buf,send_msg->size);
   agent->last_stun_binding_time=cur;
  }
  if (cur<agent->last_fresh_time || cur>=agent->last_fresh_time+300000) {
   agent->last_fresh_time+=500;agent_send_fresh(agent,300);
  }
 }
 if (agent->stun_state==ICE_CANDIDATE_STATE_SUCCEEDED) {
  uint32_t cur=get_cur_time();
  if (agent->use_stun_relay && agent->selected_pair) {
   if (cur<agent->last_fresh_time || cur>=agent->last_fresh_time+300000) {
    agent->last_fresh_time+=500;agent_send_fresh(agent,300);
   }
   if (cur<agent->last_permission_time || cur>=agent->last_permission_time+240000) {
    agent->last_permission_time+=500;agent_send_permission(agent,agent->selected_pair->remote,true);
   }
   if (cur<agent->last_channel_bind_time || cur>=agent->last_channel_bind_time+540000)
    agent_send_channel_bind(agent,agent->selected_pair,true);
  }
  if (selected_tcp_relay) {pthread_mutex_unlock(&agent->mutex);return 0;}
  if (cur<agent->last_peer_binding_time || cur>=agent->last_peer_binding_time+6000) {
   if (agent->use_stun_relay) agent_send_indication(agent,agent->selected_pair);
   else agent_send_bind_request(agent,&agent->send_msg,agent->selected_pair);
   agent->binding_not_response++;
   if (agent->binding_not_response>=agent->alive_binding_retries && agent->alive_binding_retries!=255) {
    LOGE("%d Peer not response for binding after %d",agent->mode,30000);
    pthread_mutex_unlock(&agent->mutex);return -6;
   }
   if (agent->binding_not_response>1) LOGW("%d Peer keepalive check fail count %d",agent->mode,agent->binding_not_response);
   agent->last_peer_binding_time=cur;
  }
 }
 if (!agent->remote_candidates_count) agent_recv_all(agent,agent->recv_buf,sizeof(agent->recv_buf));
 pthread_mutex_unlock(&agent->mutex);return 0;
}

void agent_disconnect(agent_handle_t agent) {
 if(!agent->remote_candidates_count){agent_send_fresh(agent,0);agent_send_fresh(agent,0);}
 udp_socket_close(&agent->stun_sock);
 tcp_connections_handle_t tcp=agent_take_tcp(agent);if(tcp)tcp_connections_close(tcp);
 tls_connections_handle_t tls=agent_take_tls(agent);if(tls)tls_connections_close(tls);
 agent->tcp_turn_tls=false;agent->tcp_turn_connected=false;
 pthread_mutex_lock(&agent->mutex);
 agent->remote_candidates_count=0;agent->local_candidates_count=0;
 agent->stun_state=ICE_CANDIDATE_STATE_FROZEN;agent->selected_pair=NULL;agent->candidate_pairs_num=0;
 memset(agent->candidate_pairs,0,sizeof(ice_candidate_pair_t)*agent->max_paired_candidates);
 agent->last_stun_binding_time=0;agent->last_peer_binding_time=0;agent->last_permission_time=0;agent->last_channel_bind_time=0;
 agent->use_stun_relay=false;agent->relay_ready_time=0;
 agent->local_ufrag[0]=0;agent->local_upwd[0]=0;
 agent_clear_remote_ice(agent);
 pthread_mutex_unlock(&agent->mutex);
 LOGI("agent disconnected done");
}

void agent_deinit(agent_handle_t agent) {
 if (!agent) return;
 udp_socket_close(&agent->stun_sock);
 tcp_connections_handle_t tcp=agent_take_tcp(agent);if(tcp)tcp_connections_close(tcp);
 tls_connections_handle_t tls=agent_take_tls(agent);if(tls)tls_connections_close(tls);
 agent->last_fresh_time=0;
 pthread_mutex_destroy(&agent->mutex);pthread_mutex_destroy(&agent->recv_mutex);
 agent->remote_candidates_count=0;agent->local_candidates_count=0;
 if(agent->local_candidates)media_lib_free(agent->local_candidates);
 if(agent->remote_candidates)media_lib_free(agent->remote_candidates);
 if(agent->candidate_pairs)media_lib_free(agent->candidate_pairs);
 if(agent->sort_pairs)media_lib_free(agent->sort_pairs);
 agent_clear_remote_ice(agent);media_lib_free(agent);
}
