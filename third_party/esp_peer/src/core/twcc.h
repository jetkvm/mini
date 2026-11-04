#ifndef ESP_PEER_CORE_TWCC_H
#define ESP_PEER_CORE_TWCC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "peer_types.h"

typedef struct twcc_ctx twcc_ctx_t;

twcc_ctx_t *twcc_create(uint16_t history_size);
void twcc_destroy(twcc_ctx_t *ctx);
void twcc_set_ext_id(twcc_ctx_t *ctx, uint8_t id);
uint8_t twcc_get_ext_id(twcc_ctx_t *ctx);
void twcc_set_ext_reserved(twcc_ctx_t *ctx, bool reserved);
int twcc_on_send(twcc_ctx_t *ctx, uint8_t *pkt, size_t *size, size_t cap);
void twcc_on_recv(twcc_ctx_t *ctx, const uint8_t *pkt, size_t size);
int twcc_build_feedback(twcc_ctx_t *ctx, uint8_t *out, int out_len,
                        uint32_t sender_ssrc, uint32_t media_ssrc,
                        uint32_t now_ms, uint16_t period_ms);
int twcc_parse_feedback(twcc_ctx_t *ctx, const uint8_t *pkt, int len,
                        esp_peer_twcc_report_t *report);
#endif
