#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t netif_input_calls;
    uint32_t netif_input_accepted;
    uint32_t pbuf_allocation_failures;
    uint32_t tcpip_enqueue_failures;
    uint32_t netif_down_drops;
    uint32_t invalid_input_drops;
    uint32_t other_input_errors;
    uint32_t emac_rx_task_priority;
} jetkvm_ethernet_rx_stats_t;

/**
 * Ethernet netstack input wrapper used in place of ethernetif_input().
 *
 * The return values of ESP-IDF's ethernetif_input() identify the two software
 * RX loss boundaries which are invisible to the EMAC missed-frame counters:
 * ESP_ERR_NO_MEM is the custom-pbuf allocation, while ESP_FAIL on a valid, up
 * interface is the non-blocking tcpip_inpkt() enqueue path.
 */
esp_err_t jetkvm_ethernet_input(void *netif, void *buffer, size_t len,
                                void *l2_buffer);

void jetkvm_ethernet_rx_stats_set_task_priority(uint32_t priority);
void jetkvm_ethernet_rx_stats_snapshot(jetkvm_ethernet_rx_stats_t *stats);

#ifdef __cplusplus
}
#endif
