#include "jetkvm_cloud_client.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_cloud_core.h"
#include "jetkvm_cloud_signal.h"
#include "jetkvm_cloud_transport_adapter.h"
#include "jetkvm_memory_policy.h"
#include "sdkconfig.h"

#define CLOUD_RX_QUEUE_DEPTH             32U
#define CLOUD_TX_QUEUE_DEPTH             32U
/* Keep the 32-message depth for ICE-candidate bursts, but cap RX+TX allocation
 * capacity together. One worker-owned message per direction may sit outside
 * this queue budget while it is parsed or sent; both still live inside the
 * fixed control-plane arena. */
#define CLOUD_QUEUE_TOTAL_BYTE_BUDGET     \
    JETKVM_MEMORY_CONTROL_CLOUD_QUEUE_MAX_BYTES
/* The client's queues, locks and event group are used only from tasks, never
 * from an ISR, so they can live in PSRAM, as the worker's stack does. In
 * internal RAM they took 1.9 KB, which every TLS admission counts against. */
#define CLOUD_SYNC_CAPS                  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CLOUD_WORKER_STACK_SIZE          8192U
#define CLOUD_WORKER_PRIORITY            6U
#define CLOUD_WEBSOCKET_TASK_STACK       6144U
#define CLOUD_WEBSOCKET_TASK_PRIORITY    5U
#define CLOUD_WEBSOCKET_BUFFER_SIZE      2048U
#define CLOUD_PING_INTERVAL_SEC          15U
#define CLOUD_DEFAULT_PONG_TIMEOUT_SEC   30U
#define CLOUD_DEFAULT_RECONNECT_MS       5000U
#define CLOUD_LOCAL_RETRY_MS             1000U
#define CLOUD_NETWORK_TIMEOUT_MS         10000U
#define CLOUD_SEND_TIMEOUT_MS            5000U
#define CLOUD_IDENTITY_RETRY_FIRST_MS     250U
#define CLOUD_IDENTITY_RETRY_ATTEMPTS     7U
#define CLOUD_WORKER_EXITED_BIT          BIT0

_Static_assert(JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE ==
                   JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE,
               "cloud wire limits must remain identical");

static const char *TAG = "jetkvm_cloud";

typedef struct {
    char *data;
    size_t size;
    size_t allocation_size;
    uint32_t connection_epoch;
    uint32_t session_id;
    uint32_t generation;
} cloud_wire_message_t;

typedef enum {
    CLOUD_QUEUE_RX = 0,
    CLOUD_QUEUE_TX,
} cloud_queue_direction_t;

struct jetkvm_cloud_client {
    SemaphoreHandle_t lock;
    /* Serializes vendor start/stop. The vendor task is asynchronous, while
     * service stop may race a worker-owned reconnect attempt. */
    SemaphoreHandle_t websocket_lifecycle_lock;
    EventGroupHandle_t events;
    QueueHandle_t rx_queue;
    QueueHandle_t tx_queue;
    TaskHandle_t worker_task;
    esp_websocket_client_handle_t websocket;

    char websocket_url[JETKVM_CLOUD_CORE_MAX_URL_SIZE + 1U];
    char headers[JETKVM_CLOUD_CORE_MAX_HEADER_SIZE];
    bool uses_tls;
    uint32_t pong_timeout_sec;
    uint32_t reconnect_timeout_ms;
    jetkvm_cloud_identity_validator_t validate_identity;
    void *identity_ctx;

    bool started;
    bool stop_requested;
    bool reconnect_requested;
    /* The worker starts every attempt, when this is due (see
     * jetkvm_cloud_retry_t); attempted selects the TLS owner name. While an
     * attempt waits for a network address, the worker checks every 50 ms, so
     * the attempt starts promptly once the address arrives. */
    jetkvm_cloud_retry_t retry;
    bool attempted;
    bool waiting_for_network;
    bool create_in_progress;
    uint32_t create_connection_epoch;
    bool pending_binding_valid;
    jetkvm_cloud_transport_binding_t pending_binding;
    jetkvm_cloud_epoch_state_t epoch;
    jetkvm_cloud_reassembler_t reassembler;
    uint8_t *reassembly_buffer;
    jetkvm_memory_policy_reservation_t websocket_memory_reservation;
    jetkvm_cloud_client_stats_t stats;
};

static bool request_reconnect_locked(jetkvm_cloud_client_handle_t client);

static void secure_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (bytes && size--) {
        *bytes++ = 0;
    }
}

static void *cloud_payload_alloc(size_t size)
{
    /* Signaling and cJSON share a bounded private control-plane arena. They
     * can fail closed without fragmenting the multi-megabyte video heap or
     * consuming internal/DMA safety reserves. */
    return jetkvm_memory_policy_control_alloc(size);
}

static void cloud_wire_message_release(cloud_wire_message_t *message)
{
    if (!message) {
        return;
    }
    if (message->data) {
        jetkvm_memory_policy_control_free(message->data);
    }
    memset(message, 0, sizeof(*message));
}

static QueueHandle_t cloud_queue(
    jetkvm_cloud_client_handle_t client, cloud_queue_direction_t direction)
{
    return direction == CLOUD_QUEUE_RX ? client->rx_queue : client->tx_queue;
}

static TaskHandle_t worker_snapshot(jetkvm_cloud_client_handle_t client)
{
    TaskHandle_t task = NULL;
    if (client && xSemaphoreTake(client->lock, portMAX_DELAY) == pdTRUE) {
        task = client->worker_task;
        xSemaphoreGive(client->lock);
    }
    return task;
}

static void notify_worker(jetkvm_cloud_client_handle_t client)
{
    TaskHandle_t task = worker_snapshot(client);
    if (task) {
        xTaskNotifyGive(task);
    }
}

static void release_websocket_memory_reservation(
    jetkvm_cloud_client_handle_t client)
{
    jetkvm_memory_policy_reservation_t reservation = {0};
    if (client && client->lock &&
        xSemaphoreTake(client->lock, portMAX_DELAY) == pdTRUE) {
        reservation = client->websocket_memory_reservation;
        memset(&client->websocket_memory_reservation, 0,
               sizeof(client->websocket_memory_reservation));
        xSemaphoreGive(client->lock);
    }
    jetkvm_memory_policy_release(&reservation);
}

static bool enqueue_owned_locked(
    jetkvm_cloud_client_handle_t client, cloud_queue_direction_t direction,
    cloud_wire_message_t *message)
{
    QueueHandle_t queue = cloud_queue(client, direction);
    if (!queue || !message || !message->data || message->size == 0) {
        return false;
    }
    uint32_t *queued_bytes = direction == CLOUD_QUEUE_RX ?
        &client->stats.rx_queued_bytes : &client->stats.tx_queued_bytes;
    uint32_t *high_watermark = direction == CLOUD_QUEUE_RX ?
        &client->stats.rx_queued_bytes_high_watermark :
        &client->stats.tx_queued_bytes_high_watermark;
    uint64_t *budget_drops = direction == CLOUD_QUEUE_RX ?
        &client->stats.rx_queue_budget_dropped :
        &client->stats.tx_queue_budget_dropped;
    const size_t charge = message->allocation_size != 0U ?
        message->allocation_size : message->size + 1U;
    const size_t total_queued = (size_t)client->stats.rx_queued_bytes +
                                client->stats.tx_queued_bytes;
    if (total_queued > CLOUD_QUEUE_TOTAL_BYTE_BUDGET ||
        charge > CLOUD_QUEUE_TOTAL_BYTE_BUDGET - total_queued) {
        (*budget_drops)++;
        return false;
    }
    if (xQueueSend(queue, message, 0) != pdTRUE) {
        return false;
    }
    *queued_bytes += (uint32_t)charge;
    if (*queued_bytes > *high_watermark) {
        *high_watermark = *queued_bytes;
    }
    memset(message, 0, sizeof(*message));
    return true;
}

static bool enqueue_owned(
    jetkvm_cloud_client_handle_t client, cloud_queue_direction_t direction,
    cloud_wire_message_t *message)
{
    xSemaphoreTake(client->lock, portMAX_DELAY);
    bool queued = enqueue_owned_locked(client, direction, message);
    xSemaphoreGive(client->lock);
    return queued;
}

static bool enqueue_copy_locked(
    jetkvm_cloud_client_handle_t client, cloud_queue_direction_t direction,
    const uint8_t *data, size_t size, uint32_t connection_epoch,
    uint32_t session_id, uint32_t generation)
{
    if (!data || size == 0 || size > JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE) {
        return false;
    }
    cloud_wire_message_t message = {
        .data = cloud_payload_alloc(size + 1U),
        .size = size,
        .allocation_size = size + 1U,
        .connection_epoch = connection_epoch,
        .session_id = session_id,
        .generation = generation,
    };
    if (!message.data) {
        return false;
    }
    memcpy(message.data, data, size);
    message.data[size] = '\0';
    if (!enqueue_owned_locked(client, direction, &message)) {
        cloud_wire_message_release(&message);
        return false;
    }
    return true;
}

static bool enqueue_copy(
    jetkvm_cloud_client_handle_t client, cloud_queue_direction_t direction,
    const uint8_t *data, size_t size, uint32_t connection_epoch,
    uint32_t session_id, uint32_t generation)
{
    if (!data || size == 0 || size > JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE) {
        return false;
    }
    cloud_wire_message_t message = {
        .data = cloud_payload_alloc(size + 1U),
        .size = size,
        .allocation_size = size + 1U,
        .connection_epoch = connection_epoch,
        .session_id = session_id,
        .generation = generation,
    };
    if (!message.data) {
        return false;
    }
    memcpy(message.data, data, size);
    message.data[size] = '\0';
    if (!enqueue_owned(client, direction, &message)) {
        cloud_wire_message_release(&message);
        return false;
    }
    return true;
}

static bool dequeue_owned(
    jetkvm_cloud_client_handle_t client, cloud_queue_direction_t direction,
    cloud_wire_message_t *message)
{
    if (xQueueReceive(cloud_queue(client, direction), message, 0) != pdTRUE) {
        return false;
    }
    xSemaphoreTake(client->lock, portMAX_DELAY);
    uint32_t *queued_bytes = direction == CLOUD_QUEUE_RX ?
        &client->stats.rx_queued_bytes : &client->stats.tx_queued_bytes;
    const size_t charge = message->allocation_size != 0U ?
        message->allocation_size : message->size + 1U;
    if (charge <= *queued_bytes) {
        *queued_bytes -= (uint32_t)charge;
    } else {
        ESP_LOGE(TAG, "cloud queue byte accounting underflow direction=%d",
                 (int)direction);
        *queued_bytes = 0U;
    }
    xSemaphoreGive(client->lock);
    return true;
}

static void drain_queue(jetkvm_cloud_client_handle_t client,
                        cloud_queue_direction_t direction)
{
    cloud_wire_message_t message;
    while (client && cloud_queue(client, direction) &&
           dequeue_owned(client, direction, &message)) {
        cloud_wire_message_release(&message);
    }
}

static void websocket_data_event(jetkvm_cloud_client_handle_t client,
                                 const esp_websocket_event_data_t *event)
{
    if (!client || !event || event->data_len < 0 || event->payload_len < 0 ||
        event->payload_offset < 0) {
        return;
    }

    bool queued = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (!client->started || client->stop_requested) {
        xSemaphoreGive(client->lock);
        return;
    }
    if (event->op_code == JETKVM_CLOUD_WS_OPCODE_PONG) {
        client->stats.pongs_received++;
    }
    jetkvm_cloud_reassembly_result_t result =
        jetkvm_cloud_reassembler_push(
            &client->reassembler, event->op_code, event->fin,
            (size_t)event->payload_len, (size_t)event->payload_offset,
            (const uint8_t *)event->data_ptr, (size_t)event->data_len);
    if (result == JETKVM_CLOUD_REASSEMBLY_COMPLETE) {
        const uint8_t *data = jetkvm_cloud_reassembler_message_data(
            &client->reassembler);
        size_t size = jetkvm_cloud_reassembler_message_size(
            &client->reassembler);
        if (size > 0 && client->epoch.connected) {
            queued = enqueue_copy_locked(
                client, CLOUD_QUEUE_RX, data, size,
                client->epoch.connection_epoch, 0, 0);
            if (queued) {
                client->stats.rx_messages++;
                client->stats.rx_bytes += size;
            } else {
                client->stats.rx_dropped++;
            }
        }
        jetkvm_cloud_reassembler_reset(&client->reassembler);
    } else if (result == JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR) {
        client->stats.rx_protocol_errors++;
        client->stats.rx_dropped++;
    } else if (result == JETKVM_CLOUD_REASSEMBLY_TOO_LARGE) {
        client->stats.rx_oversize++;
        client->stats.rx_dropped++;
    }
    xSemaphoreGive(client->lock);
    if (queued) {
        notify_worker(client);
    }
}

static void websocket_event_handler(void *handler_arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    (void)base;
    jetkvm_cloud_client_handle_t client = handler_arg;
    const esp_websocket_event_data_t *event = event_data;
    if (!client) {
        return;
    }

    bool notify = false;
    bool purge_queues = false;
    jetkvm_memory_policy_reservation_t completed_connect_memory = {0};
    if (event_id == WEBSOCKET_EVENT_DATA) {
        websocket_data_event(client, event);
        return;
    }

    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (!client->started || client->stop_requested) {
        xSemaphoreGive(client->lock);
        return;
    }
    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        completed_connect_memory = client->websocket_memory_reservation;
        memset(&client->websocket_memory_reservation, 0,
               sizeof(client->websocket_memory_reservation));
        uint32_t epoch = jetkvm_cloud_epoch_connected(&client->epoch);
        jetkvm_cloud_reassembler_reset(&client->reassembler);
        client->stats.connections++;
        client->stats.state = JETKVM_CLOUD_CLIENT_CONNECTED;
        ESP_LOGI(TAG, "device WSS connected epoch=%" PRIu32, epoch);
        notify = true;
        purge_queues = true;
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH:
        completed_connect_memory = client->websocket_memory_reservation;
        memset(&client->websocket_memory_reservation, 0,
               sizeof(client->websocket_memory_reservation));
        if (client->epoch.connected) {
            uint32_t epoch = client->epoch.connection_epoch;
            jetkvm_cloud_epoch_disconnected(&client->epoch, epoch);
            jetkvm_cloud_reassembler_reset(&client->reassembler);
            client->stats.disconnections++;
            client->stats.state = JETKVM_CLOUD_CLIENT_RECONNECTING;
            ESP_LOGI(TAG, "device WSS disconnected epoch=%" PRIu32, epoch);
            notify = true;
            purge_queues = true;
        }
        if (request_reconnect_locked(client)) {
            notify = true;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        client->stats.websocket_errors++;
        if (event) {
            ESP_LOGW(TAG, "device WSS error type=%d status=%d",
                     event->error_handle.error_type,
                     event->error_handle.esp_ws_handshake_status_code);
        } else {
            ESP_LOGW(TAG, "device WSS error without detail");
        }
        break;
    default:
        break;
    }
    xSemaphoreGive(client->lock);
    jetkvm_memory_policy_release(&completed_connect_memory);
    if (purge_queues) {
        /* No event from the new epoch can precede CONNECTED, and callbacks
         * cannot enqueue after DISCONNECTED fails the epoch match. */
        drain_queue(client, CLOUD_QUEUE_RX);
        drain_queue(client, CLOUD_QUEUE_TX);
    }
    if (notify) {
        notify_worker(client);
    }
}

static bool cloud_signal_callback(
    uint32_t session_id, uint32_t generation,
    webrtc_transport_cloud_signal_type_t type,
    const char *data, size_t size, void *ctx)
{
    jetkvm_cloud_client_handle_t client = ctx;
    if (!client || !data || size == 0) {
        return false;
    }

    uint32_t connection_epoch = 0;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    bool current = !client->stop_requested && !client->reconnect_requested &&
        client->epoch.session_active &&
        client->epoch.session_id == session_id &&
        client->epoch.generation == generation &&
        jetkvm_cloud_epoch_matches_session(
            &client->epoch, client->epoch.session_connection_epoch,
            session_id, generation);
    if (current) {
        connection_epoch = client->epoch.session_connection_epoch;
    }
    xSemaphoreGive(client->lock);
    if (!current) {
        return false;
    }

    cloud_wire_message_t message = {
        .data = cloud_payload_alloc(
            JETKVM_CLOUD_SIGNAL_MAX_WIRE_BUFFER_SIZE),
        .allocation_size = JETKVM_CLOUD_SIGNAL_MAX_WIRE_BUFFER_SIZE,
        .connection_epoch = connection_epoch,
        .session_id = session_id,
        .generation = generation,
    };
    if (!message.data) {
        return false;
    }

    size_t output_length = 0;
    jetkvm_cloud_signal_status_t status = JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    if (type == WEBRTC_TRANSPORT_CLOUD_SIGNAL_ANSWER) {
        status = jetkvm_cloud_signal_encode_answer(
            data, size, message.data,
            JETKVM_CLOUD_SIGNAL_MAX_WIRE_BUFFER_SIZE, &output_length);
    } else if (type == WEBRTC_TRANSPORT_CLOUD_SIGNAL_CANDIDATE) {
        const char *candidate = data;
        size_t candidate_length = size;
        while (candidate_length &&
               (candidate[candidate_length - 1U] == '\r' ||
                candidate[candidate_length - 1U] == '\n' ||
                candidate[candidate_length - 1U] == ' ' ||
                candidate[candidate_length - 1U] == '\t')) {
            candidate_length--;
        }
        if (candidate_length >= 2U && candidate[0] == 'a' &&
            candidate[1] == '=') {
            candidate += 2;
            candidate_length -= 2U;
        }
        const jetkvm_cloud_signal_candidate_view_t view = {
            .candidate = candidate,
            .candidate_length = candidate_length,
            .sdp_mid = "0",
            .sdp_mid_length = 1,
            .sdp_mline_index = 0,
        };
        status = jetkvm_cloud_signal_encode_candidate(
            &view, message.data,
            JETKVM_CLOUD_SIGNAL_MAX_WIRE_BUFFER_SIZE, &output_length);
    }
    if (status != JETKVM_CLOUD_SIGNAL_OK || output_length == 0) {
        cloud_wire_message_release(&message);
        xSemaphoreTake(client->lock, portMAX_DELAY);
        client->stats.signaling_errors++;
        client->stats.tx_dropped++;
        xSemaphoreGive(client->lock);
        return false;
    }
    message.size = output_length;
    message.data[output_length] = '\0';
    const size_t compact_size = output_length + 1U;
    void *compact = jetkvm_memory_policy_control_realloc(
        message.data, compact_size);
    if (compact != NULL) {
        message.data = compact;
        message.allocation_size = compact_size;
    }
    if (!enqueue_owned(client, CLOUD_QUEUE_TX, &message)) {
        cloud_wire_message_release(&message);
        xSemaphoreTake(client->lock, portMAX_DELAY);
        client->stats.tx_dropped++;
        xSemaphoreGive(client->lock);
        return false;
    }
    notify_worker(client);
    return true;
}

static bool request_reconnect_locked(jetkvm_cloud_client_handle_t client)
{
    if (client->stop_requested || client->reconnect_requested) {
        return false;
    }
    client->reconnect_requested = true;
    client->stats.reconnect_requests++;
    client->stats.state = JETKVM_CLOUD_CLIENT_RECONNECTING;
    return true;
}

static void request_reconnect(jetkvm_cloud_client_handle_t client)
{
    xSemaphoreTake(client->lock, portMAX_DELAY);
    bool notify = request_reconnect_locked(client);
    xSemaphoreGive(client->lock);
    if (notify) {
        notify_worker(client);
    }
}

static void cloud_lifecycle_callback(
    const webrtc_transport_cloud_lifecycle_event_t *event, void *ctx)
{
    jetkvm_cloud_client_handle_t client = ctx;
    if (!client || !event) {
        return;
    }
    bool notify = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (client->epoch.session_active &&
        client->epoch.session_id == event->session_id) {
        if (event->type ==
                WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_GENERATION_INVALIDATED &&
            jetkvm_cloud_epoch_replace_generation(
                &client->epoch, event->session_id, event->generation,
                event->replacement_generation)) {
            notify = request_reconnect_locked(client) || notify;
        } else if (event->type ==
                       WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_SESSION_CLOSED &&
                   client->epoch.generation == event->generation) {
            jetkvm_cloud_epoch_clear_session(
                &client->epoch, event->session_id, event->generation);
            notify = true;
        }
    } else if (client->create_in_progress &&
               client->create_connection_epoch != 0 &&
               event->session_id != 0) {
        /* create() can wake the peer task before its returned binding is
         * published below. Capture that early lifecycle generation so a mode
         * invalidation cannot orphan an uncloseable controlled session. */
        if (event->type == WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_PEER_READY &&
            event->generation != 0) {
            client->pending_binding = (jetkvm_cloud_transport_binding_t) {
                .session_id = event->session_id,
                .generation = event->generation,
            };
            client->pending_binding_valid = true;
        } else if (event->type ==
                       WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_GENERATION_INVALIDATED &&
                   event->replacement_generation != 0) {
            client->pending_binding = (jetkvm_cloud_transport_binding_t) {
                .session_id = event->session_id,
                .generation = event->replacement_generation,
            };
            client->pending_binding_valid = true;
            notify = request_reconnect_locked(client) || notify;
        } else if (event->type ==
                       WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_SESSION_CLOSED &&
                   client->pending_binding_valid &&
                   client->pending_binding.session_id == event->session_id) {
            client->pending_binding_valid = false;
            memset(&client->pending_binding, 0,
                   sizeof(client->pending_binding));
        }
    }
    xSemaphoreGive(client->lock);
    if (notify) {
        /* Lifecycle callbacks must never synchronously close the peer/WSS. */
        notify_worker(client);
    }
}

static bool snapshot_binding(jetkvm_cloud_client_handle_t client,
                             jetkvm_cloud_transport_binding_t *binding)
{
    bool active = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (client->epoch.session_active) {
        binding->session_id = client->epoch.session_id;
        binding->generation = client->epoch.generation;
        active = true;
    }
    xSemaphoreGive(client->lock);
    return active;
}

static void close_bound_session(jetkvm_cloud_client_handle_t client)
{
    /* A generation invalidation can win while close waits for the serialized
     * callback gate. Retry only the newly published replacement generation. */
    for (unsigned attempt = 0; attempt < 10U; ++attempt) {
        jetkvm_cloud_transport_binding_t binding = { 0 };
        if (!snapshot_binding(client, &binding)) {
            return;
        }
        esp_err_t result = jetkvm_cloud_transport_close(&binding);
        if (result == ESP_ERR_INVALID_STATE) {
            /* Do not erase the only binding to a replacement generation before
             * its serialized INVALIDATED callback has had a chance to run. */
            vTaskDelay(1);
            continue;
        }
        xSemaphoreTake(client->lock, portMAX_DELAY);
        jetkvm_cloud_epoch_clear_session(
            &client->epoch, binding.session_id, binding.generation);
        bool still_active = client->epoch.session_active;
        xSemaphoreGive(client->lock);
        if (!still_active) {
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "controlled session close failed: %s",
                         esp_err_to_name(result));
            }
            return;
        }
    }
    /* A future offer replaces any transport-side orphan. Clear the adapter's
     * stale binding so it cannot accept callbacks or candidates in this epoch. */
    jetkvm_cloud_transport_binding_t stale = { 0 };
    if (snapshot_binding(client, &stale)) {
        xSemaphoreTake(client->lock, portMAX_DELAY);
        jetkvm_cloud_epoch_clear_session(
            &client->epoch, stale.session_id, stale.generation);
        xSemaphoreGive(client->lock);
    }
    ESP_LOGW(TAG, "controlled session did not reach a closeable generation");
}

static bool connection_current(jetkvm_cloud_client_handle_t client,
                               uint32_t connection_epoch)
{
    bool current = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    current = !client->stop_requested && !client->reconnect_requested &&
        jetkvm_cloud_epoch_is_current_connection(
            &client->epoch, connection_epoch);
    xSemaphoreGive(client->lock);
    return current;
}

static void record_signaling_error(jetkvm_cloud_client_handle_t client)
{
    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->stats.signaling_errors++;
    client->stats.rx_dropped++;
    xSemaphoreGive(client->lock);
}

static void route_offer(jetkvm_cloud_client_handle_t client,
                        const cloud_wire_message_t *message,
                        jetkvm_cloud_signal_offer_t *offer)
{
    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->stats.offers_received++;
    xSemaphoreGive(client->lock);
    if (!offer->has_oidc_google || offer->oidc_google.length == 0) {
        jetkvm_cloud_signal_offer_free(offer);
        record_signaling_error(client);
        return;
    }

    jetkvm_cloud_identity_validation_t identity =
        JETKVM_CLOUD_IDENTITY_INVALID;
    uint32_t retry_delay_ms = CLOUD_IDENTITY_RETRY_FIRST_MS;
    for (unsigned attempt = 0U;
         attempt < CLOUD_IDENTITY_RETRY_ATTEMPTS; ++attempt) {
        if (!connection_current(client, message->connection_epoch)) {
            jetkvm_cloud_signal_offer_free(offer);
            return;
        }
        identity = client->validate_identity(
            offer->oidc_google.data, offer->oidc_google.length,
            client->identity_ctx);
        if (identity != JETKVM_CLOUD_IDENTITY_RETRYABLE) {
            break;
        }
        xSemaphoreTake(client->lock, portMAX_DELAY);
        client->stats.offers_deferred_identity++;
        xSemaphoreGive(client->lock);
        if (attempt + 1U == CLOUD_IDENTITY_RETRY_ATTEMPTS ||
            !connection_current(client, message->connection_epoch)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        if (retry_delay_ms <= UINT32_MAX / 2U) {
            retry_delay_ms *= 2U;
        }
    }
    if (identity != JETKVM_CLOUD_IDENTITY_VALID) {
        xSemaphoreTake(client->lock, portMAX_DELAY);
        if (identity == JETKVM_CLOUD_IDENTITY_RETRYABLE) {
            client->stats.offers_retry_exhausted_identity++;
        } else {
            client->stats.offers_rejected_identity++;
        }
        client->stats.rx_dropped++;
        xSemaphoreGive(client->lock);
        jetkvm_cloud_signal_offer_free(offer);
        if (identity == JETKVM_CLOUD_IDENTITY_RETRYABLE &&
            connection_current(client, message->connection_epoch)) {
            /* The server's offer is one-shot. Reconnect after bounded local
             * contention so it can be offered again instead of misclassifying
             * resource pressure as an authentication failure. */
            request_reconnect(client);
        }
        return;
    }
    if (!connection_current(client, message->connection_epoch)) {
        jetkvm_cloud_signal_offer_free(offer);
        return;
    }

    /* Authenticate before replacing an existing peer. */
    close_bound_session(client);
    if (!connection_current(client, message->connection_epoch)) {
        jetkvm_cloud_signal_offer_free(offer);
        return;
    }

    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->create_in_progress = true;
    client->create_connection_epoch = message->connection_epoch;
    client->pending_binding_valid = false;
    memset(&client->pending_binding, 0, sizeof(client->pending_binding));
    xSemaphoreGive(client->lock);

    jetkvm_cloud_transport_binding_t binding = { 0 };
    esp_err_t result = jetkvm_cloud_transport_create(
        offer, cloud_signal_callback, cloud_lifecycle_callback,
        client, &binding);
    jetkvm_cloud_transport_binding_t effective_binding = binding;
    bool invalidated_during_create = false;
    bool bound = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->create_in_progress = false;
    client->create_connection_epoch = 0;
    if (client->pending_binding_valid &&
        client->pending_binding.session_id == binding.session_id) {
        effective_binding = client->pending_binding;
        invalidated_during_create =
            effective_binding.generation != binding.generation;
    }
    client->pending_binding_valid = false;
    memset(&client->pending_binding, 0, sizeof(client->pending_binding));
    if (result == ESP_OK) {
        bound = jetkvm_cloud_epoch_bind_session(
            &client->epoch, message->connection_epoch,
            effective_binding.session_id, effective_binding.generation);
    }
    xSemaphoreGive(client->lock);
    if (result != ESP_OK) {
        jetkvm_cloud_signal_offer_free(offer);
        record_signaling_error(client);
        if (jetkvm_cloud_core_authenticated_step_requires_reconnect(
                true, false)) {
            request_reconnect(client);
        }
        ESP_LOGW(TAG, "controlled session create failed: %s",
                 esp_err_to_name(result));
        return;
    }
    if (!bound) {
        (void)jetkvm_cloud_transport_close(&effective_binding);
        jetkvm_cloud_signal_offer_free(offer);
        if (jetkvm_cloud_core_authenticated_step_requires_reconnect(
                true, false)) {
            request_reconnect(client);
        }
        return;
    }
    if (invalidated_during_create) {
        jetkvm_cloud_signal_offer_free(offer);
        request_reconnect(client);
        return;
    }

    result = jetkvm_cloud_transport_submit_offer(
        &effective_binding, offer->sdp.data, offer->sdp.length);
    jetkvm_cloud_signal_offer_free(offer);
    if (result != ESP_OK) {
        record_signaling_error(client);
        if (jetkvm_cloud_core_authenticated_step_requires_reconnect(
                true, false)) {
            request_reconnect(client);
        }
        close_bound_session(client);
        ESP_LOGW(TAG, "controlled offer submit failed: %s",
                 esp_err_to_name(result));
        return;
    }
    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->stats.offers_accepted++;
    xSemaphoreGive(client->lock);
}

static void route_candidate(jetkvm_cloud_client_handle_t client,
                            const cloud_wire_message_t *message,
                            jetkvm_cloud_signal_candidate_t *candidate)
{
    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->stats.candidates_received++;
    xSemaphoreGive(client->lock);

    /* The answer bundles every m-section on m-line 0's transport, and the
     * agent cannot resolve mDNS names. Other candidates only fill esp_peer's
     * ten remote slots, which the browser's peer-reflexive address needs. */
    if (candidate->sdp_mline_index != 0 ||
        strstr(candidate->candidate.data, ".local ") != NULL) {
        jetkvm_cloud_signal_candidate_free(candidate);
        return;
    }

    jetkvm_cloud_transport_binding_t binding = { 0 };
    bool current = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (!client->reconnect_requested &&
        client->epoch.session_active &&
        client->epoch.session_connection_epoch == message->connection_epoch &&
        jetkvm_cloud_epoch_is_current_connection(
            &client->epoch, message->connection_epoch)) {
        binding.session_id = client->epoch.session_id;
        binding.generation = client->epoch.generation;
        current = true;
    }
    xSemaphoreGive(client->lock);
    if (!current) {
        jetkvm_cloud_signal_candidate_free(candidate);
        record_signaling_error(client);
        return;
    }
    esp_err_t result = jetkvm_cloud_transport_submit_candidate(
        &binding, candidate->candidate.data, candidate->candidate.length);
    jetkvm_cloud_signal_candidate_free(candidate);
    if (result != ESP_OK) {
        record_signaling_error(client);
        if (jetkvm_cloud_core_authenticated_step_requires_reconnect(
                true, false)) {
            request_reconnect(client);
        }
        return;
    }
    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->stats.candidates_accepted++;
    xSemaphoreGive(client->lock);
}

static void queue_application_pong(jetkvm_cloud_client_handle_t client,
                                   uint32_t connection_epoch)
{
    static const uint8_t pong[] = "pong";
    if (!enqueue_copy(client, CLOUD_QUEUE_TX, pong, sizeof(pong) - 1U,
                      connection_epoch, 0, 0)) {
        xSemaphoreTake(client->lock, portMAX_DELAY);
        client->stats.tx_dropped++;
        xSemaphoreGive(client->lock);
        return;
    }
    notify_worker(client);
}

static void route_rx_message(jetkvm_cloud_client_handle_t client,
                             const cloud_wire_message_t *message)
{
    if (!connection_current(client, message->connection_epoch)) {
        return;
    }
    if (message->size == 4U && memcmp(message->data, "ping", 4U) == 0) {
        queue_application_pong(client, message->connection_epoch);
        return;
    }
    /* Both protocol parsers run their raw depth/UTF-8/NUL/size prescan before
     * cJSON allocation. Never classify an attacker-controlled 64 KiB envelope
     * with a naked JSON parser on this 8 KiB worker stack. */
    jetkvm_cloud_signal_offer_t offer = { 0 };
    if (jetkvm_cloud_signal_parse_offer(
            (const uint8_t *)message->data, message->size, &offer) ==
        JETKVM_CLOUD_SIGNAL_OK) {
        route_offer(client, message, &offer);
        return;
    }
    jetkvm_cloud_signal_candidate_t candidate = { 0 };
    if (jetkvm_cloud_signal_parse_candidate(
            (const uint8_t *)message->data, message->size, &candidate) ==
        JETKVM_CLOUD_SIGNAL_OK) {
        route_candidate(client, message, &candidate);
        return;
    }
    record_signaling_error(client);
}

static bool tx_message_current(jetkvm_cloud_client_handle_t client,
                               const cloud_wire_message_t *message,
                               esp_websocket_client_handle_t *websocket)
{
    bool current = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    current = !client->stop_requested && !client->reconnect_requested &&
        jetkvm_cloud_epoch_is_current_connection(
            &client->epoch, message->connection_epoch);
    if (current && message->session_id != 0) {
        current = jetkvm_cloud_epoch_matches_session(
            &client->epoch, message->connection_epoch,
            message->session_id, message->generation);
    }
    *websocket = current ? client->websocket : NULL;
    xSemaphoreGive(client->lock);
    return current && *websocket &&
           esp_websocket_client_is_connected(*websocket);
}

static void send_tx_message(jetkvm_cloud_client_handle_t client,
                            const cloud_wire_message_t *message)
{
    esp_websocket_client_handle_t websocket = NULL;
    if (!tx_message_current(client, message, &websocket)) {
        xSemaphoreTake(client->lock, portMAX_DELAY);
        client->stats.tx_dropped++;
        xSemaphoreGive(client->lock);
        return;
    }
    int sent = esp_websocket_client_send_text(
        websocket, message->data, (int)message->size,
        pdMS_TO_TICKS(CLOUD_SEND_TIMEOUT_MS));
    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (sent == (int)message->size) {
        client->stats.tx_messages++;
        client->stats.tx_bytes += message->size;
    } else {
        client->stats.tx_send_errors++;
        client->stats.tx_dropped++;
        (void)request_reconnect_locked(client);
    }
    xSemaphoreGive(client->lock);
}

static bool session_is_stale(jetkvm_cloud_client_handle_t client)
{
    bool stale = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    stale = client->epoch.session_active &&
        (!client->epoch.connected ||
         client->epoch.session_connection_epoch !=
             client->epoch.connection_epoch);
    xSemaphoreGive(client->lock);
    return stale;
}

static bool reconnect_is_requested(jetkvm_cloud_client_handle_t client)
{
    bool requested = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    requested = client->reconnect_requested && !client->stop_requested;
    xSemaphoreGive(client->lock);
    return requested;
}

/* Ends the current attempt or connection and schedules the next attempt
 * reconnect_timeout_ms later, as upstream waits 5 s after a websocket error
 * or disconnect. The session is closed at once, not when the retry is due. */
static void stop_websocket_for_retry(jetkvm_cloud_client_handle_t client)
{
    close_bound_session(client);
    drain_queue(client, CLOUD_QUEUE_RX);
    drain_queue(client, CLOUD_QUEUE_TX);

    esp_websocket_client_handle_t websocket = NULL;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    websocket = client->websocket;
    client->reconnect_requested = false;
    client->stats.state = JETKVM_CLOUD_CLIENT_RECONNECTING;
    xSemaphoreGive(client->lock);
    if (!websocket) {
        return;
    }

    xSemaphoreTake(client->websocket_lifecycle_lock, portMAX_DELAY);
    esp_err_t stop_result = esp_websocket_client_stop(websocket);
    xSemaphoreGive(client->websocket_lifecycle_lock);
    if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "device WSS stop for reconnect failed: %s",
                 esp_err_to_name(stop_result));
    }

    /* stop() is synchronous; any previous attempt's unmaterialized ticket is
     * no longer needed even if the component omitted a terminal event. */
    release_websocket_memory_reservation(client);

    xSemaphoreTake(client->lock, portMAX_DELAY);
    /* A CLOSED/FINISH callback from the intentional stop may have queued a
     * reconnect. This function has already scheduled that reconnect. */
    client->reconnect_requested = false;
    if (!client->stop_requested) {
        jetkvm_cloud_retry_request(&client->retry, esp_timer_get_time(),
                                   client->reconnect_timeout_ms);
    }
    xSemaphoreGive(client->lock);
}

/* Upstream waits for the network before it dials the cloud. An address on the
 * default interface is the Mini's equivalent; it does not prove that DNS or
 * the cloud answer, so a failed attempt still waits for the retry delay. */
static bool cloud_network_up(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip = {0};
    return netif && esp_netif_is_netif_up(netif) &&
           esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0U;
}

static void retry_locally(jetkvm_cloud_client_handle_t client)
{
    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (!client->stop_requested) {
        jetkvm_cloud_retry_request(&client->retry, esp_timer_get_time(),
                                   CLOUD_LOCAL_RETRY_MS);
    }
    xSemaphoreGive(client->lock);
}

static void start_websocket_if_due(jetkvm_cloud_client_handle_t client)
{
    xSemaphoreTake(client->lock, portMAX_DELAY);
    bool pending = client->retry.pending && !client->stop_requested;
    xSemaphoreGive(client->lock);
    if (!pending) {
        return;
    }
    bool network_up = cloud_network_up();

    xSemaphoreTake(client->lock, portMAX_DELAY);
    esp_websocket_client_handle_t websocket = client->websocket;
    bool due = websocket && !client->stop_requested &&
        jetkvm_cloud_retry_due(&client->retry, esp_timer_get_time(),
                               network_up);
    bool log_network_wait = !network_up && !client->waiting_for_network;
    client->waiting_for_network = !network_up;
    const char *owner = client->attempted ? "cloud-wss-connect" : "cloud-wss";
    xSemaphoreGive(client->lock);
    if (log_network_wait) {
        ESP_LOGI(TAG, "device WSS waiting for a network address");
    }
    if (!due) {
        return;
    }

    /* The connection's reservation is held until CONNECTED or a terminal
     * failed-attempt event, which can be the network timeout when the cloud
     * accepts TCP and does not answer. A session's open still fits beside
     * it. Automatic reconnect is disabled, so every handshake is admitted
     * here before the component creates its worker. */
    jetkvm_memory_policy_reservation_t connect_memory = {0};
    if (!jetkvm_memory_policy_reserve_tls(owner, &connect_memory)) {
        ESP_LOGW(TAG, "device WSS connect deferred: TLS memory unavailable");
        retry_locally(client);
        return;
    }

    xSemaphoreTake(client->lock, portMAX_DELAY);
    bool stopping = client->stop_requested;
    if (!stopping) {
        client->websocket_memory_reservation = connect_memory;
        memset(&connect_memory, 0, sizeof(connect_memory));
        /* Published before start, so a CONNECTED or failure event that
         * arrives during start cannot be overwritten by this attempt. */
        jetkvm_cloud_retry_started(&client->retry);
        client->attempted = true;
    }
    xSemaphoreGive(client->lock);
    if (stopping) {
        jetkvm_memory_policy_release(&connect_memory);
        return;
    }

    /* stop_requested is published before public stop takes this mutex. If it
     * won the race, do not start a new vendor task. If start won, public stop
     * blocks here until the vendor's running state is fully published. */
    xSemaphoreTake(client->websocket_lifecycle_lock, portMAX_DELAY);
    xSemaphoreTake(client->lock, portMAX_DELAY);
    stopping = client->stop_requested;
    xSemaphoreGive(client->lock);
    esp_err_t start_result = stopping ? ESP_ERR_INVALID_STATE :
        esp_websocket_client_start(websocket);
    xSemaphoreGive(client->websocket_lifecycle_lock);
    if (stopping) {
        release_websocket_memory_reservation(client);
        return;
    }
    if (start_result != ESP_OK) {
        release_websocket_memory_reservation(client);
        ESP_LOGW(TAG, "device WSS start failed: %s",
                 esp_err_to_name(start_result));
        retry_locally(client);
    }
}

static bool stop_is_requested(jetkvm_cloud_client_handle_t client)
{
    bool stop = false;
    xSemaphoreTake(client->lock, portMAX_DELAY);
    stop = client->stop_requested;
    xSemaphoreGive(client->lock);
    return stop;
}

static void cloud_worker_task(void *arg)
{
    jetkvm_cloud_client_handle_t client = arg;
    for (;;) {
        xSemaphoreTake(client->lock, portMAX_DELAY);
        uint32_t tick_ms = client->waiting_for_network ? 50U : 250U;
        xSemaphoreGive(client->lock);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(tick_ms));
        if (stop_is_requested(client)) {
            break;
        }
        if (session_is_stale(client)) {
            close_bound_session(client);
        }
        if (reconnect_is_requested(client)) {
            stop_websocket_for_retry(client);
            continue;
        }
        start_websocket_if_due(client);

        cloud_wire_message_t message;
        while (dequeue_owned(client, CLOUD_QUEUE_RX, &message)) {
            route_rx_message(client, &message);
            cloud_wire_message_release(&message);
            if (stop_is_requested(client) || reconnect_is_requested(client)) {
                break;
            }
        }
        if (reconnect_is_requested(client)) {
            stop_websocket_for_retry(client);
            continue;
        }
        while (dequeue_owned(client, CLOUD_QUEUE_TX, &message)) {
            send_tx_message(client, &message);
            cloud_wire_message_release(&message);
            if (stop_is_requested(client) || reconnect_is_requested(client)) {
                break;
            }
        }
    }

    close_bound_session(client);
    drain_queue(client, CLOUD_QUEUE_RX);
    drain_queue(client, CLOUD_QUEUE_TX);
    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->worker_task = NULL;
    xSemaphoreGive(client->lock);
    xEventGroupSetBits(client->events, CLOUD_WORKER_EXITED_BIT);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t jetkvm_cloud_client_init(
    const jetkvm_cloud_client_config_t *config,
    jetkvm_cloud_client_handle_t *client_out)
{
    if (!config || !client_out || !config->url || !config->device_id ||
        !config->app_version || !config->token ||
        !config->validate_identity) {
        return ESP_ERR_INVALID_ARG;
    }
    *client_out = NULL;
    jetkvm_cloud_client_handle_t client =
        jetkvm_memory_policy_control_alloc(sizeof(*client));
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    memset(client, 0, sizeof(*client));

    size_t header_length = 0;
    jetkvm_cloud_core_status_t core_status =
        jetkvm_cloud_core_make_ws_url(
            config->url, client->websocket_url,
            sizeof(client->websocket_url), &client->uses_tls);
    if (core_status == JETKVM_CLOUD_CORE_OK) {
        core_status = jetkvm_cloud_core_make_headers(
            config->device_id, config->app_version, config->sku,
            config->token, client->headers, sizeof(client->headers),
            &header_length);
    }
    if (core_status != JETKVM_CLOUD_CORE_OK || header_length == 0) {
        secure_zero(client, sizeof(*client));
        jetkvm_memory_policy_control_free(client);
        return ESP_ERR_INVALID_ARG;
    }

    client->pong_timeout_sec = config->pong_timeout_sec ?
        config->pong_timeout_sec : CLOUD_DEFAULT_PONG_TIMEOUT_SEC;
    client->reconnect_timeout_ms = config->reconnect_timeout_ms ?
        config->reconnect_timeout_ms : CLOUD_DEFAULT_RECONNECT_MS;
    if (client->pong_timeout_sec <= CLOUD_PING_INTERVAL_SEC ||
        client->pong_timeout_sec > 3600U ||
        client->reconnect_timeout_ms > 3600000U) {
        secure_zero(client, sizeof(*client));
        jetkvm_memory_policy_control_free(client);
        return ESP_ERR_INVALID_ARG;
    }
    client->validate_identity = config->validate_identity;
    client->identity_ctx = config->identity_ctx;
    client->lock = xSemaphoreCreateMutexWithCaps(CLOUD_SYNC_CAPS);
    client->websocket_lifecycle_lock =
        xSemaphoreCreateMutexWithCaps(CLOUD_SYNC_CAPS);
    client->events = xEventGroupCreateWithCaps(CLOUD_SYNC_CAPS);
    client->rx_queue = xQueueCreateWithCaps(
        CLOUD_RX_QUEUE_DEPTH, sizeof(cloud_wire_message_t), CLOUD_SYNC_CAPS);
    client->tx_queue = xQueueCreateWithCaps(
        CLOUD_TX_QUEUE_DEPTH, sizeof(cloud_wire_message_t), CLOUD_SYNC_CAPS);
    client->reassembly_buffer = cloud_payload_alloc(
        JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE + 1U);
    if (!client->lock || !client->websocket_lifecycle_lock ||
        !client->events || !client->rx_queue ||
        !client->tx_queue || !client->reassembly_buffer) {
        if (client->rx_queue) {
            vQueueDeleteWithCaps(client->rx_queue);
        }
        if (client->tx_queue) {
            vQueueDeleteWithCaps(client->tx_queue);
        }
        if (client->events) {
            vEventGroupDeleteWithCaps(client->events);
        }
        if (client->lock) {
            vSemaphoreDeleteWithCaps(client->lock);
        }
        if (client->websocket_lifecycle_lock) {
            vSemaphoreDeleteWithCaps(client->websocket_lifecycle_lock);
        }
        if (client->reassembly_buffer) {
            jetkvm_memory_policy_control_free(client->reassembly_buffer);
        }
        secure_zero(client, sizeof(*client));
        jetkvm_memory_policy_control_free(client);
        return ESP_ERR_NO_MEM;
    }
    jetkvm_cloud_reassembler_init(
        &client->reassembler, client->reassembly_buffer,
        JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE + 1U);
    client->stats.state = JETKVM_CLOUD_CLIENT_STOPPED;
    *client_out = client;
    return ESP_OK;
}

esp_err_t jetkvm_cloud_client_start(jetkvm_cloud_client_handle_t client)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (client->uses_tls) {
        ESP_LOGE(TAG, "WSS requires the ESP certificate bundle");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (client->started) {
        xSemaphoreGive(client->lock);
        return ESP_ERR_INVALID_STATE;
    }
    client->stop_requested = false;
    client->reconnect_requested = false;
    /* The worker makes the first attempt as soon as the network is up. */
    jetkvm_cloud_retry_request(&client->retry, esp_timer_get_time(), 0U);
    client->attempted = false;
    client->waiting_for_network = false;
    client->started = true;
    client->stats.started = true;
    client->stats.state = JETKVM_CLOUD_CLIENT_CONNECTING;
    xEventGroupClearBits(client->events, CLOUD_WORKER_EXITED_BIT);
    xSemaphoreGive(client->lock);

    const esp_websocket_client_config_t websocket_config = {
        .uri = client->websocket_url,
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .user_context = client,
        .task_prio = CLOUD_WEBSOCKET_TASK_PRIORITY,
        .task_name = "jk_cloud_ws",
        .task_stack = CLOUD_WEBSOCKET_TASK_STACK,
        .buffer_size = CLOUD_WEBSOCKET_BUFFER_SIZE,
        .headers = client->headers,
        .pingpong_timeout_sec = (int)client->pong_timeout_sec,
        .disable_pingpong_discon = false,
        .crt_bundle_attach = client->uses_tls ? esp_crt_bundle_attach : NULL,
        .skip_cert_common_name_check = false,
        .reconnect_timeout_ms = (int)client->reconnect_timeout_ms,
        .network_timeout_ms = CLOUD_NETWORK_TIMEOUT_MS,
        .ping_interval_sec = CLOUD_PING_INTERVAL_SEC,
    };
    esp_websocket_client_handle_t websocket =
        esp_websocket_client_init(&websocket_config);
    if (!websocket) {
        xSemaphoreTake(client->lock, portMAX_DELAY);
        client->started = false;
        client->stats.started = false;
        client->stats.state = JETKVM_CLOUD_CLIENT_STOPPED;
        xSemaphoreGive(client->lock);
        release_websocket_memory_reservation(client);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = esp_websocket_register_events(
        websocket, WEBSOCKET_EVENT_ANY, websocket_event_handler, client);
    if (result != ESP_OK) {
        esp_websocket_client_destroy(websocket);
        xSemaphoreTake(client->lock, portMAX_DELAY);
        client->started = false;
        client->stats.started = false;
        client->stats.state = JETKVM_CLOUD_CLIENT_STOPPED;
        xSemaphoreGive(client->lock);
        release_websocket_memory_reservation(client);
        return result;
    }

    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->websocket = websocket;
    xSemaphoreGive(client->lock);
    BaseType_t task_result = xTaskCreateWithCaps(
        cloud_worker_task, "jk_cloud", CLOUD_WORKER_STACK_SIZE,
        client, CLOUD_WORKER_PRIORITY, &client->worker_task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_result != pdPASS) {
        esp_websocket_unregister_events(
            websocket, WEBSOCKET_EVENT_ANY, websocket_event_handler);
        esp_websocket_client_destroy(websocket);
        xSemaphoreTake(client->lock, portMAX_DELAY);
        client->websocket = NULL;
        client->started = false;
        client->stats.started = false;
        client->stats.state = JETKVM_CLOUD_CLIENT_STOPPED;
        xSemaphoreGive(client->lock);
        release_websocket_memory_reservation(client);
        return ESP_ERR_NO_MEM;
    }
    notify_worker(client);
    return ESP_OK;
}

esp_err_t jetkvm_cloud_client_stop(jetkvm_cloud_client_handle_t client)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(client->lock, portMAX_DELAY);
    if (!client->started) {
        xSemaphoreGive(client->lock);
        return ESP_OK;
    }
    client->stop_requested = true;
    esp_websocket_client_handle_t websocket = client->websocket;
    xSemaphoreGive(client->lock);

    esp_err_t stop_result = ESP_OK;
    if (websocket) {
        xSemaphoreTake(client->websocket_lifecycle_lock, portMAX_DELAY);
        stop_result = esp_websocket_client_stop(websocket);
        xSemaphoreGive(client->websocket_lifecycle_lock);
        if (stop_result == ESP_ERR_INVALID_STATE) {
            stop_result = ESP_OK;
        }
    }
    notify_worker(client);
    xEventGroupWaitBits(client->events, CLOUD_WORKER_EXITED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    if (websocket) {
        (void)esp_websocket_unregister_events(
            websocket, WEBSOCKET_EVENT_ANY, websocket_event_handler);
        esp_err_t destroy_result = esp_websocket_client_destroy(websocket);
        if (stop_result == ESP_OK) {
            stop_result = destroy_result;
        }
    }
    drain_queue(client, CLOUD_QUEUE_RX);
    drain_queue(client, CLOUD_QUEUE_TX);
    xSemaphoreTake(client->lock, portMAX_DELAY);
    client->websocket = NULL;
    client->started = false;
    client->stop_requested = false;
    client->reconnect_requested = false;
    client->epoch.connected = false;
    client->stats.started = false;
    client->stats.state = JETKVM_CLOUD_CLIENT_STOPPED;
    jetkvm_cloud_reassembler_reset(&client->reassembler);
    xSemaphoreGive(client->lock);
    release_websocket_memory_reservation(client);
    return stop_result;
}

esp_err_t jetkvm_cloud_client_destroy(jetkvm_cloud_client_handle_t client)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = jetkvm_cloud_client_stop(client);
    if (result != ESP_OK) {
        return result;
    }
    drain_queue(client, CLOUD_QUEUE_RX);
    drain_queue(client, CLOUD_QUEUE_TX);
    vQueueDeleteWithCaps(client->rx_queue);
    vQueueDeleteWithCaps(client->tx_queue);
    vEventGroupDeleteWithCaps(client->events);
    if (client->reassembly_buffer) {
        secure_zero(client->reassembly_buffer,
                    JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE + 1U);
        jetkvm_memory_policy_control_free(client->reassembly_buffer);
        client->reassembly_buffer = NULL;
    }
    SemaphoreHandle_t lock = client->lock;
    SemaphoreHandle_t websocket_lifecycle_lock =
        client->websocket_lifecycle_lock;
    jetkvm_memory_policy_reservation_t websocket_reservation =
        client->websocket_memory_reservation;
    memset(&client->websocket_memory_reservation, 0,
           sizeof(client->websocket_memory_reservation));
    client->lock = NULL;
    client->websocket_lifecycle_lock = NULL;
    secure_zero(client, sizeof(*client));
    vSemaphoreDeleteWithCaps(lock);
    vSemaphoreDeleteWithCaps(websocket_lifecycle_lock);
    jetkvm_memory_policy_release(&websocket_reservation);
    jetkvm_memory_policy_control_free(client);
    return ESP_OK;
}

esp_err_t jetkvm_cloud_client_get_stats(
    jetkvm_cloud_client_handle_t client,
    jetkvm_cloud_client_stats_t *stats)
{
    if (!client || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(client->lock, portMAX_DELAY);
    *stats = client->stats;
    stats->started = client->started;
    stats->connected = client->epoch.connected;
    stats->session_active = client->epoch.session_active;
    stats->connection_epoch = client->epoch.connection_epoch;
    stats->session_id = client->epoch.session_id;
    stats->generation = client->epoch.generation;
    xSemaphoreGive(client->lock);
    return ESP_OK;
}
