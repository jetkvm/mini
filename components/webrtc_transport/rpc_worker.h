/* Bounded background JSON-RPC operations; replies belong to the originating peer.
 * Control (Cloud, factory reset, USB emulation), media (virtual media, USB
 * configuration), guest (OS Services clipboard and file drop), MQTT
 * (connection test) and update (update checks) methods have separate queues,
 * so a slow USB re-enumeration cannot hold a factory reset back and a
 * clipboard request waiting for its sender, an MQTT test waiting for its
 * broker or an update check waiting for the update server holds back
 * neither. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "jetkvm_control.h"

typedef struct {
    uint32_t session_id;
    uint32_t generation;
    uint16_t stream_id;
    uint32_t access_epoch;
    bool cloud_authority;
} rpc_worker_origin_t;

typedef struct {
    uint32_t session_id;
    uint32_t generation;
    uint16_t stream_id;
    bool publish_deregistration;
    bool will_reboot;
    size_t length;
    /* A guest reply too long for json, up to JETKVM_MEMORY_RPC_SCRATCH_BYTES,
     * is in this buffer instead; NULL otherwise. */
    char *large;
    char json[2048];
} rpc_worker_reply_t;

/* The caller has admitted the request for this session and access epoch; the
 * worker checks the epoch again before it starts. false means the method is
 * not a worker method, or its queue is full or could not start: the caller
 * then dispatches with rpc_worker_peer_fallbacks() installed. */
bool rpc_worker_defer(const uint8_t *input, size_t length,
                      const rpc_worker_origin_t *origin);
/* A taken reply's large buffer belongs to the caller, who frees it with
 * rpc_worker_release_reply() once the send was attempted, sent or not. */
bool rpc_worker_take_reply(rpc_worker_reply_t *reply);
/* Frees reply->large, if any, and clears it; a second call does nothing. */
void rpc_worker_release_reply(rpc_worker_reply_t *reply);
void rpc_worker_cancel_replies(void);

/* The reply's reply->length bytes. */
static inline const char *rpc_worker_reply_text(const rpc_worker_reply_t *reply)
{
    return reply->large ? reply->large : reply->json;
}

/* Replace every worker method's handler in a peer-task context with one that
 * reports BUSY (its queue holds jobs) or unavailable, never the blocking
 * operation. The OS Services clipboard and file drop callbacks and the MQTT
 * connection test report the same as reasons. */
void rpc_worker_peer_fallbacks(jetkvm_control_context_t *context);

/* Provided by the transport: wake the peer task to send a finished reply. */
void rpc_worker_notify_done(void);
