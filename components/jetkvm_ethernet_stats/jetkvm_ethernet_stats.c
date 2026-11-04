#include "jetkvm_ethernet_stats.h"

#include <stdatomic.h>

#include "esp_netif.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/netif.h"

typedef struct {
    atomic_uint_least32_t netif_input_calls;
    atomic_uint_least32_t pbuf_allocation_failures;
    atomic_uint_least32_t tcpip_enqueue_failures;
    atomic_uint_least32_t netif_down_drops;
    atomic_uint_least32_t invalid_input_drops;
    atomic_uint_least32_t other_input_errors;
    atomic_uint_least32_t emac_rx_task_priority;
} ethernet_rx_counters_t;

static ethernet_rx_counters_t s_rx;

esp_err_t jetkvm_ethernet_input(void *netif_handle, void *buffer, size_t len,
                                void *l2_buffer)
{
    atomic_fetch_add_explicit(&s_rx.netif_input_calls, 1,
                              memory_order_relaxed);

    const bool invalid = netif_handle == NULL || buffer == NULL;
    const bool interface_up = !invalid &&
        netif_is_up((const struct netif *)netif_handle);
    const esp_err_t result =
        ethernetif_input(netif_handle, buffer, len, l2_buffer);

    if (result == ESP_OK) {
        /* Accepted is derived in the snapshot, avoiding another atomic
         * operation on every successful frame. */
    } else if (invalid) {
        atomic_fetch_add_explicit(&s_rx.invalid_input_drops, 1,
                                  memory_order_relaxed);
    } else if (!interface_up) {
        atomic_fetch_add_explicit(&s_rx.netif_down_drops, 1,
                                  memory_order_relaxed);
    } else if (result == ESP_ERR_NO_MEM) {
        atomic_fetch_add_explicit(&s_rx.pbuf_allocation_failures, 1,
                                  memory_order_relaxed);
    } else if (result == ESP_FAIL) {
        /* With a valid buffer and an up Ethernet interface, ESP-IDF reaches
         * this return only when tcpip_input()/tcpip_inpkt() cannot allocate a
         * message or non-blockingly post it to the tcpip mailbox. */
        atomic_fetch_add_explicit(&s_rx.tcpip_enqueue_failures, 1,
                                  memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&s_rx.other_input_errors, 1,
                                  memory_order_relaxed);
    }
    return result;
}

void jetkvm_ethernet_rx_stats_set_task_priority(uint32_t priority)
{
    atomic_store_explicit(&s_rx.emac_rx_task_priority, priority,
                          memory_order_relaxed);
}

void jetkvm_ethernet_rx_stats_snapshot(jetkvm_ethernet_rx_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
#define LOAD_COUNTER(name) \
    stats->name = atomic_load_explicit(&s_rx.name, memory_order_relaxed)
    LOAD_COUNTER(netif_input_calls);
    LOAD_COUNTER(pbuf_allocation_failures);
    LOAD_COUNTER(tcpip_enqueue_failures);
    LOAD_COUNTER(netif_down_drops);
    LOAD_COUNTER(invalid_input_drops);
    LOAD_COUNTER(other_input_errors);
    LOAD_COUNTER(emac_rx_task_priority);
#undef LOAD_COUNTER
    const uint32_t classified_drops = stats->pbuf_allocation_failures +
        stats->tcpip_enqueue_failures + stats->netif_down_drops +
        stats->invalid_input_drops + stats->other_input_errors;
    stats->netif_input_accepted =
        classified_drops <= stats->netif_input_calls ?
            stats->netif_input_calls - classified_drops : 0U;
}
