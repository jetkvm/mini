#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    int64_t expiry_epoch, control_epoch;
    uint64_t control_received_ms, node_id;
    bool expired;
} ml_key_expiry_t;
bool ml_key_expiry_update(ml_key_expiry_t *state, const cJSON *map, uint64_t received_ms);
bool ml_key_expiry_expired(const ml_key_expiry_t *state, uint64_t now_ms, int64_t local_epoch);
