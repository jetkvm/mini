#include "ml_key_expiry.h"
#include "ml_timestamp.h"
#include "ml_peer_map.h"

bool ml_key_expiry_update(ml_key_expiry_t *state, const cJSON *map, uint64_t received_ms)
{
    if (!state || !cJSON_IsObject(map)) return false;
    const cJSON *clock = cJSON_GetObjectItemCaseSensitive(map, "ControlTime");
    if (clock && !cJSON_IsNull(clock)) {
        int64_t epoch;
        if (!cJSON_IsString(clock) || !ml_timestamp_parse(clock->valuestring, &epoch) || epoch <= 0)
            goto invalid;
        state->control_epoch = epoch;
        state->control_received_ms = received_ms;
    }
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(map, "Node");
    if (node && !cJSON_IsNull(node)) {
        if (!cJSON_IsObject(node)) goto invalid;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "ID");
        if (id && !ml_node_id_parse(id, &state->node_id)) goto invalid;
        const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(node, "KeyExpiry");
        const cJSON *expired = cJSON_GetObjectItemCaseSensitive(node, "Expired");
        int64_t epoch = 0;
        if (expiry && (!cJSON_IsString(expiry) || !ml_timestamp_parse(expiry->valuestring, &epoch)))
            goto invalid;
        if (expired && !cJSON_IsBool(expired)) goto invalid;
        state->expiry_epoch = epoch;
        state->expired = cJSON_IsTrue(expired);
    }
    /* Headscale also sends the local node's expiry as a PeerChange delta. */
    const cJSON *patches = cJSON_GetObjectItemCaseSensitive(map, "PeersChangedPatch");
    if (patches && !cJSON_IsNull(patches)) {
        if (!cJSON_IsArray(patches)) goto invalid;
        const cJSON *patch;
        cJSON_ArrayForEach(patch, patches) {
            uint64_t id;
            if (!cJSON_IsObject(patch) || !ml_node_id_parse(
                    cJSON_GetObjectItemCaseSensitive(patch, "NodeID"), &id)) goto invalid;
            if (id != state->node_id) continue;
            const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(patch, "KeyExpiry");
            if (!expiry || cJSON_IsNull(expiry)) continue;
            int64_t epoch;
            if (!cJSON_IsString(expiry) || !ml_timestamp_parse(expiry->valuestring, &epoch))
                goto invalid;
            state->expiry_epoch = epoch;
            state->expired = false;
        }
    }
    return true;
invalid:
    state->expired = true;
    return false;
}

bool ml_key_expiry_expired(const ml_key_expiry_t *state, uint64_t now_ms, int64_t local_epoch)
{
    if (!state || state->expired) return true;
    if (!state->expiry_epoch) return false;
    int64_t current;
    if (state->control_epoch > 0) {
        if (now_ms < state->control_received_ms) return true;
        current = state->control_epoch + (int64_t)((now_ms - state->control_received_ms) / 1000);
    } else {
        /* Until SNTP establishes a clock, an expiring key cannot be verified. */
        if (local_epoch < INT64_C(1577836800)) return true;
        current = local_epoch;
    }
    return current >= state->expiry_epoch;
}
