/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_usb.h"

#include <string.h>

#include "device/usbd_pvt.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "jetkvm_os_services.h"
#include "jetkvm_usb_hid.h"
#include "os_services_link.h"
#include "sdkconfig.h"
#include "tusb.h"

static const char *TAG = "osvc_usb";

#define INTERFACE_CLASS 0xffU
#define INTERFACE_SUBCLASS 0x4aU
#define INTERFACE_PROTOCOL 0x01U
#define REQUEST_TYPE 0xc1U
#define REQUEST_RESET 0x40U
#define REQUEST_STATUS 0x41U
#define PROTOCOL_MAJOR 5U

/* Each OUT read and each IN transfer: DMA-capable PSRAM, which the USB DMA
 * reads and writes. Internal RAM is kept for the Wi-Fi driver's receive
 * buffers and WebRTC. */
#define BUFFER_BYTES (CONFIG_JETKVM_OS_SERVICES_USB_BUFFER_KIB * 1024U)
#define RX_BUFFERS 2U
/* Queued IN messages, in PSRAM: a whole 64 KiB message and then some. */
#define TX_RING_BYTES (128U * 1024U)

_Static_assert(BUFFER_BYTES % 512U == 0U,
               "a read must be a whole number of high-speed packets");
_Static_assert(BUFFER_BYTES <= UINT16_MAX,
               "TinyUSB takes 16-bit transfer lengths");
_Static_assert((TX_RING_BYTES & (TX_RING_BYTES - 1U)) == 0U,
               "the IN ring's capacity must be a power of two");

typedef enum {
    RX_FREE = 0,
    RX_ARMED,   /* a read on the bus */
    RX_FILLED,  /* queued for the service task */
    RX_READING, /* the service task has it */
} rx_state_t;

static struct {
    portMUX_TYPE lock;
    TaskHandle_t task;
    /* The buffers exist. Without them the interface reports Faulted. */
    bool ready;

    /* TinyUSB's task only. */
    osvc_rx_framer_t framer;
    uint32_t rx_requested;
    uint8_t rx_armed_index;
    uint8_t *rx_buffer[RX_BUFFERS];
    uint8_t *tx_bounce;

    /* Under lock. */
    uint8_t rhport;
    uint8_t ep_out;
    uint8_t ep_in;
    bool configured;
    bool high_speed;
    bool rx_armed;
    bool rx_paused;
    rx_state_t rx_state[RX_BUFFERS];
    uint32_t rx_length[RX_BUFFERS];
    uint8_t rx_queue[RX_BUFFERS];
    uint8_t rx_queue_count;
    uint8_t rx_reading_index;
    /* When each read filled, and when the service task gave the last one
     * back. */
    int64_t rx_filled_us[RX_BUFFERS];
    int64_t rx_released_us;
    uint32_t reset_sequence;
    uint8_t token[OSVC_TOKEN_LEN];
    uint32_t reconfigure_sequence;
    uint32_t restart_reconfigure_sequence;
    bool recovery_restart;
    uint32_t reset_reconfigure_sequence;
    uint8_t status[OSVC_USB_STATUS_LEN];
    int64_t in_started_us;
    osvc_usb_counters_t counters;

    /* The service task produces, TinyUSB's task consumes. */
    osvc_tx_ring_t ring;
    SemaphoreHandle_t tx_space;

    /* Calls into TinyUSB from the service task: TinyUSB is running while
     * stack_alive is set, under stack_mutex. */
    SemaphoreHandle_t stack_mutex;
    bool stack_alive;
    uint32_t kick_pending;
} s_usb = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .rx_paused = true,
};

static void notify_task(void)
{
    if (s_usb.task != NULL) {
        xTaskNotifyGive(s_usb.task);
    }
}

static void make_status(uint8_t status[OSVC_USB_STATUS_LEN], osvc_phase_t phase,
                        const uint8_t token[OSVC_TOKEN_LEN])
{
    memset(status, 0, OSVC_USB_STATUS_LEN);
    memcpy(status, "JKV5", 4U);
    status[4] = (uint8_t)phase;
    memcpy(status + 8, token, OSVC_TOKEN_LEN);
}

/* Drops the OUT reads waiting for the service task. Under lock. */
static void drop_filled_locked(void)
{
    for (size_t i = 0U; i < RX_BUFFERS; ++i) {
        if (s_usb.rx_state[i] == RX_FILLED) {
            s_usb.rx_state[i] = RX_FREE;
            s_usb.counters.dropped_reads++;
        }
    }
    s_usb.rx_queue_count = 0U;
}

/* --- TinyUSB's task ---------------------------------------------------- */

static void rx_arm(void)
{
    if (!s_usb.ready) {
        return;
    }
    int index = -1;
    uint8_t rhport = 0U;
    uint8_t endpoint = 0U;
    portENTER_CRITICAL(&s_usb.lock);
    if (s_usb.configured && !s_usb.rx_paused && !s_usb.rx_armed) {
        for (size_t i = 0U; i < RX_BUFFERS; ++i) {
            if (s_usb.rx_state[i] == RX_FREE) {
                index = (int)i;
                break;
            }
        }
    }
    if (index >= 0) {
        s_usb.rx_state[index] = RX_ARMED;
        s_usb.rx_armed = true;
        rhport = s_usb.rhport;
        endpoint = s_usb.ep_out;
    }
    portEXIT_CRITICAL(&s_usb.lock);
    if (index < 0) {
        return;
    }
    s_usb.rx_armed_index = (uint8_t)index;
    s_usb.rx_requested = osvc_rx_framer_next_length(&s_usb.framer);
    if (!usbd_edpt_xfer(rhport, endpoint, s_usb.rx_buffer[index],
                        (uint16_t)s_usb.rx_requested)) {
        portENTER_CRITICAL(&s_usb.lock);
        s_usb.rx_state[index] = RX_FREE;
        s_usb.rx_armed = false;
        portEXIT_CRITICAL(&s_usb.lock);
    }
}

static void rx_completed(xfer_result_t result, uint32_t received)
{
    const uint8_t index = s_usb.rx_armed_index;
    portENTER_CRITICAL(&s_usb.lock);
    const bool expected = s_usb.rx_armed && s_usb.rx_state[index] == RX_ARMED;
    portEXIT_CRITICAL(&s_usb.lock);
    if (!expected) {
        return;
    }
    if (result == XFER_RESULT_SUCCESS) {
        osvc_rx_framer_completed(&s_usb.framer, s_usb.rx_buffer[index],
                                 s_usb.rx_requested, received);
    } else {
        osvc_rx_framer_restart(&s_usb.framer);
        received = 0U;
    }
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.rx_armed = false;
    if (received > 0U) {
        s_usb.rx_state[index] = RX_FILLED;
        s_usb.rx_length[index] = received;
        s_usb.rx_filled_us[index] = esp_timer_get_time();
        s_usb.rx_queue[s_usb.rx_queue_count++] = index;
        s_usb.counters.out_bytes += received;
        s_usb.counters.out_transfers++;
    } else {
        s_usb.rx_state[index] = RX_FREE;
    }
    portEXIT_CRITICAL(&s_usb.lock);
    if (received > 0U) {
        notify_task();
    }
    rx_arm();
}

static void tx_start(void)
{
    if (!s_usb.ready) {
        return;
    }
    portENTER_CRITICAL(&s_usb.lock);
    const bool configured = s_usb.configured;
    const uint8_t rhport = s_usb.rhport;
    const uint8_t endpoint = s_usb.ep_in;
    portEXIT_CRITICAL(&s_usb.lock);
    if (!configured) {
        return;
    }
    uint32_t length = 0U;
    const osvc_tx_action_t action =
        osvc_tx_ring_start(&s_usb.ring, s_usb.tx_bounce, BUFFER_BYTES, &length);
    if (action == OSVC_TX_IDLE) {
        return;
    }
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.in_started_us = esp_timer_get_time();
    if (action == OSVC_TX_ZERO_LENGTH) {
        s_usb.counters.zero_length_packets++;
    }
    portEXIT_CRITICAL(&s_usb.lock);
    if (!usbd_edpt_xfer(rhport, endpoint, s_usb.tx_bounce, (uint16_t)length)) {
        osvc_tx_ring_abort(&s_usb.ring);
        portENTER_CRITICAL(&s_usb.lock);
        s_usb.in_started_us = 0;
        portEXIT_CRITICAL(&s_usb.lock);
    }
}

static void tx_completed(uint32_t sent)
{
    osvc_tx_ring_completed(&s_usb.ring);
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.in_started_us = 0;
    s_usb.counters.in_bytes += sent;
    s_usb.counters.in_transfers++;
    portEXIT_CRITICAL(&s_usb.lock);
    xSemaphoreGive(s_usb.tx_space);
    tx_start();
}

static void kick_from_usb_task(void *param)
{
    (void)param;
    __atomic_store_n(&s_usb.kick_pending, 0U, __ATOMIC_RELEASE);
    rx_arm();
    tx_start();
}

/* Transfers on the bus are gone: the endpoints closed or TinyUSB stopped. */
static void transfers_lost(void)
{
    if (s_usb.ready) {
        osvc_tx_ring_abort(&s_usb.ring);
    }
    osvc_rx_framer_restart(&s_usb.framer);
    portENTER_CRITICAL(&s_usb.lock);
    for (size_t i = 0U; i < RX_BUFFERS; ++i) {
        if (s_usb.rx_state[i] == RX_ARMED) {
            s_usb.rx_state[i] = RX_FREE;
        }
    }
    s_usb.rx_armed = false;
    s_usb.in_started_us = 0;
    portEXIT_CRITICAL(&s_usb.lock);
}

static void driver_init(void)
{
    /* A new TinyUSB task: a kick the old one never ran is gone. */
    __atomic_store_n(&s_usb.kick_pending, 0U, __ATOMIC_RELEASE);
    transfers_lost();
    if (s_usb.stack_mutex != NULL) {
        xSemaphoreTake(s_usb.stack_mutex, portMAX_DELAY);
        s_usb.stack_alive = true;
        xSemaphoreGive(s_usb.stack_mutex);
    }
}

static bool driver_deinit(void)
{
    /* stack_stopping already ran; this covers an uninstall without it. */
    __atomic_store_n(&s_usb.stack_alive, false, __ATOMIC_RELEASE);
    return true;
}

static void driver_reset(uint8_t rhport)
{
    (void)rhport;
    transfers_lost();
    portENTER_CRITICAL(&s_usb.lock);
    const bool was_configured = s_usb.configured;
    s_usb.configured = false;
    s_usb.ep_out = 0U;
    s_usb.ep_in = 0U;
    if (was_configured) {
        s_usb.reconfigure_sequence++;
        s_usb.counters.reconfigures++;
    }
    portEXIT_CRITICAL(&s_usb.lock);
    if (was_configured) {
        notify_task();
    }
}

static uint16_t driver_open(uint8_t rhport, tusb_desc_interface_t const *itf,
                            uint16_t max_length)
{
    if (itf->bInterfaceClass != INTERFACE_CLASS ||
        itf->bInterfaceSubClass != INTERFACE_SUBCLASS ||
        itf->bInterfaceProtocol != INTERFACE_PROTOCOL ||
        itf->bNumEndpoints != 2U) {
        return 0U;
    }
    const uint8_t *descriptor = tu_desc_next(itf);
    const uint8_t *end = (const uint8_t *)itf + max_length;
    uint8_t ep_out = 0U;
    uint8_t ep_in = 0U;
    uint16_t packet_size = 0U;
    for (size_t found = 0U; found < 2U; ++found) {
        if (descriptor + sizeof(tusb_desc_endpoint_t) > end ||
            tu_desc_type(descriptor) != TUSB_DESC_ENDPOINT) {
            return 0U;
        }
        const tusb_desc_endpoint_t *endpoint =
            (const tusb_desc_endpoint_t *)descriptor;
        if (endpoint->bmAttributes.xfer != TUSB_XFER_BULK ||
            !usbd_edpt_open(rhport, endpoint)) {
            return 0U;
        }
        if (tu_edpt_dir(endpoint->bEndpointAddress) == TUSB_DIR_IN) {
            ep_in = endpoint->bEndpointAddress;
        } else {
            ep_out = endpoint->bEndpointAddress;
        }
        packet_size = tu_edpt_packet_size(endpoint);
        descriptor = tu_desc_next(descriptor);
    }
    if (ep_in == 0U || ep_out == 0U || packet_size == 0U) {
        return 0U;
    }

    osvc_rx_framer_init(&s_usb.framer, packet_size, BUFFER_BYTES);
    if (s_usb.ready) {
        s_usb.ring.packet_size = packet_size;
    }
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.rhport = rhport;
    s_usb.ep_out = ep_out;
    s_usb.ep_in = ep_in;
    s_usb.high_speed = tud_speed_get() == TUSB_SPEED_HIGH;
    s_usb.configured = s_usb.ready;
    s_usb.reconfigure_sequence++;
    s_usb.counters.reconfigures++;
    portEXIT_CRITICAL(&s_usb.lock);
    notify_task();
    return (uint16_t)(descriptor - (const uint8_t *)itf);
}

static bool driver_control(uint8_t rhport, uint8_t stage,
                           tusb_control_request_t const *request)
{
    (void)rhport;
    (void)stage;
    (void)request;
    return false;
}

/* A failed read lost its bytes, and the host may have read part of a failed
 * IN transfer, so the stream cannot go on: the session ends as on a bus
 * reset, and the next one needs a RESET. TinyUSB's DWC2 driver completes
 * every bulk transfer with success. */
static void transfer_failed(void)
{
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.reconfigure_sequence++;
    s_usb.counters.reconfigures++;
    portEXIT_CRITICAL(&s_usb.lock);
    notify_task();
}

static bool driver_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                        uint32_t transferred)
{
    (void)rhport;
    portENTER_CRITICAL(&s_usb.lock);
    const uint8_t ep_out = s_usb.ep_out;
    const uint8_t ep_in = s_usb.ep_in;
    portEXIT_CRITICAL(&s_usb.lock);
    const bool out = ep_addr == ep_out && ep_out != 0U;
    const bool in = ep_addr == ep_in && ep_in != 0U;
    if (!out && !in) {
        return false;
    }
    if (result != XFER_RESULT_SUCCESS) {
        transfer_failed();
    }
    if (out) {
        rx_completed(result, transferred);
    } else {
        tx_completed(result == XFER_RESULT_SUCCESS ? transferred : 0U);
    }
    return true;
}

static const usbd_class_driver_t s_driver = {
    .name = "OSVC",
    .init = driver_init,
    .deinit = driver_deinit,
    .reset = driver_reset,
    .open = driver_open,
    .control_xfer_cb = driver_control,
    .xfer_cb = driver_xfer,
    .xfer_isr = NULL,
    .sof = NULL,
};

/* TinyUSB tries application drivers before its own for every interface;
 * this one claims only the OS Services interface. */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1U;
    return &s_driver;
}

static bool vendor_request(uint8_t rhport, uint8_t stage, const void *raw,
                           void *ctx)
{
    (void)ctx;
    const tusb_control_request_t *request = raw;
    if (request->bmRequestType != REQUEST_TYPE ||
        (request->bRequest != REQUEST_RESET &&
         request->bRequest != REQUEST_STATUS) ||
        request->wValue != PROTOCOL_MAJOR ||
        request->wLength != OSVC_USB_STATUS_LEN) {
        portENTER_CRITICAL(&s_usb.lock);
        s_usb.counters.stalled_requests++;
        portEXIT_CRITICAL(&s_usb.lock);
        return false;
    }
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    /* The control stage sends from this buffer before the next request. */
    static uint8_t answer[OSVC_USB_STATUS_LEN];
    if (request->bRequest == REQUEST_RESET) {
        uint8_t token[OSVC_TOKEN_LEN];
        do {
            esp_fill_random(token, sizeof(token));
        } while (memcmp(token, (const uint8_t[OSVC_TOKEN_LEN]){0},
                        sizeof(token)) == 0);
        /* No session can use the reads of the previous one. */
        osvc_rx_framer_restart(&s_usb.framer);
        portENTER_CRITICAL(&s_usb.lock);
        s_usb.reset_sequence++;
        s_usb.reset_reconfigure_sequence = s_usb.reconfigure_sequence;
        memcpy(s_usb.token, token, sizeof(token));
        make_status(s_usb.status,
                    s_usb.ready ? OSVC_PHASE_RESETTING : OSVC_PHASE_FAULTED,
                    token);
        memcpy(answer, s_usb.status, sizeof(answer));
        s_usb.counters.resets++;
        portEXIT_CRITICAL(&s_usb.lock);
        notify_task();
    } else {
        portENTER_CRITICAL(&s_usb.lock);
        memcpy(answer, s_usb.status, sizeof(answer));
        s_usb.counters.statuses++;
        portEXIT_CRITICAL(&s_usb.lock);
    }
    return tud_control_xfer(rhport, request, answer, sizeof(answer));
}

static void stack_stopping(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_usb.stack_mutex, portMAX_DELAY);
    s_usb.stack_alive = false;
    xSemaphoreGive(s_usb.stack_mutex);
    portENTER_CRITICAL(&s_usb.lock);
    const bool was_configured = s_usb.configured;
    s_usb.configured = false;
    if (was_configured) {
        s_usb.reconfigure_sequence++;
        s_usb.counters.reconfigures++;
        if (!s_usb.recovery_restart) {
            s_usb.restart_reconfigure_sequence = s_usb.reconfigure_sequence;
        }
    }
    s_usb.recovery_restart = false;
    portEXIT_CRITICAL(&s_usb.lock);
    notify_task();
}

/* --- The service task --------------------------------------------------- */

static void kick(void)
{
    if (!s_usb.ready ||
        __atomic_exchange_n(&s_usb.kick_pending, 1U, __ATOMIC_ACQ_REL) != 0U) {
        return;
    }
    xSemaphoreTake(s_usb.stack_mutex, portMAX_DELAY);
    if (s_usb.stack_alive) {
        usbd_defer_func(kick_from_usb_task, NULL, false);
    } else {
        __atomic_store_n(&s_usb.kick_pending, 0U, __ATOMIC_RELEASE);
    }
    xSemaphoreGive(s_usb.stack_mutex);
}

/* A transfer buffer, in whole PSRAM cache lines. TinyUSB writes an IN
 * buffer back before the DMA reads it and drops an OUT buffer's lines after
 * the DMA wrote it, and this code only reads an OUT buffer. A line the
 * allocation left dirty would be written back over the DMA's data, so none
 * is left. */
static uint8_t *alloc_transfer_buffer(void)
{
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
    size_t alignment = 0U;
    if (esp_cache_get_alignment(caps, &alignment) != ESP_OK ||
        BUFFER_BYTES % alignment != 0U) {
        return NULL;
    }
    uint8_t *buffer = heap_caps_aligned_alloc(alignment, BUFFER_BYTES, caps);
    if (buffer != NULL &&
        esp_cache_msync(buffer, BUFFER_BYTES,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE) != ESP_OK) {
        heap_caps_free(buffer);
        buffer = NULL;
    }
    return buffer;
}

esp_err_t osvc_usb_init(TaskHandle_t task)
{
    s_usb.task = task;
    s_usb.stack_mutex = xSemaphoreCreateMutex();
    s_usb.tx_space = xSemaphoreCreateBinary();
    uint8_t *ring = heap_caps_malloc(TX_RING_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_usb.tx_bounce = alloc_transfer_buffer();
    bool buffers = ring != NULL && s_usb.tx_bounce != NULL;
    for (size_t i = 0U; i < RX_BUFFERS; ++i) {
        s_usb.rx_buffer[i] = alloc_transfer_buffer();
        buffers = buffers && s_usb.rx_buffer[i] != NULL;
    }
    const bool ready = buffers && s_usb.stack_mutex != NULL &&
                       s_usb.tx_space != NULL;
    if (ready) {
        osvc_tx_ring_init(&s_usb.ring, ring, TX_RING_BYTES, 512U);
    } else {
        ESP_LOGE(TAG, "no memory for the OS Services transfer buffers");
        free(ring);
        heap_caps_free(s_usb.tx_bounce);
        s_usb.tx_bounce = NULL;
        for (size_t i = 0U; i < RX_BUFFERS; ++i) {
            heap_caps_free(s_usb.rx_buffer[i]);
            s_usb.rx_buffer[i] = NULL;
        }
    }
    /* The protocol has no all-zero token, also before the first RESET. */
    uint8_t token[OSVC_TOKEN_LEN];
    do {
        esp_fill_random(token, sizeof(token));
    } while (memcmp(token, (const uint8_t[OSVC_TOKEN_LEN]){0},
                    sizeof(token)) == 0);
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.ready = ready;
    memcpy(s_usb.token, token, sizeof(token));
    make_status(s_usb.status,
                ready ? OSVC_PHASE_RESETTING : OSVC_PHASE_FAULTED, s_usb.token);
    portEXIT_CRITICAL(&s_usb.lock);

    const jetkvm_usb_hid_os_services_hooks_t hooks = {
        .vendor_request = vendor_request,
        .stack_stopping = s_usb.stack_mutex != NULL ? stack_stopping : NULL,
        .ctx = NULL,
    };
    esp_err_t error = jetkvm_usb_hid_set_os_services_hooks(&hooks);
    if (error == ESP_OK && !ready) {
        error = ESP_ERR_NO_MEM;
    }
    return error;
}

void jetkvm_os_services_usb_recovery_next(void)
{
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.recovery_restart = true;
    portEXIT_CRITICAL(&s_usb.lock);
}

void jetkvm_os_services_usb_recovery_done(void)
{
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.recovery_restart = false;
    portEXIT_CRITICAL(&s_usb.lock);
}

void osvc_usb_get_events(osvc_usb_events_t *events)
{
    portENTER_CRITICAL(&s_usb.lock);
    events->reset_sequence = s_usb.reset_sequence;
    memcpy(events->token, s_usb.token, sizeof(events->token));
    events->reconfigure_sequence = s_usb.reconfigure_sequence;
    events->restart_reconfigure_sequence = s_usb.restart_reconfigure_sequence;
    events->reset_reconfigure_sequence = s_usb.reset_reconfigure_sequence;
    events->configured = s_usb.configured;
    events->high_speed = s_usb.high_speed;
    portEXIT_CRITICAL(&s_usb.lock);
}

void osvc_usb_publish_status(uint32_t reset_sequence,
                             const uint8_t status[OSVC_USB_STATUS_LEN])
{
    portENTER_CRITICAL(&s_usb.lock);
    if (reset_sequence == s_usb.reset_sequence) {
        memcpy(s_usb.status, status, OSVC_USB_STATUS_LEN);
    }
    portEXIT_CRITICAL(&s_usb.lock);
}

void osvc_usb_restart(void)
{
    if (!s_usb.ready) {
        return;
    }
    portENTER_CRITICAL(&s_usb.lock);
    drop_filled_locked();
    s_usb.rx_paused = false;
    /* A transfer from the previous session waits for the new sender's
     * first read; its 2 seconds start now. */
    if (s_usb.in_started_us != 0) {
        s_usb.in_started_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_usb.lock);
    osvc_tx_ring_flush(&s_usb.ring);
    kick();
}

void osvc_usb_stop(void)
{
    if (!s_usb.ready) {
        return;
    }
    portENTER_CRITICAL(&s_usb.lock);
    s_usb.rx_paused = true;
    drop_filled_locked();
    portEXIT_CRITICAL(&s_usb.lock);
    osvc_tx_ring_flush(&s_usb.ring);
    kick();
}

/* The wait of the read the service task takes now. Under lock. */
static void count_read_wait_locked(uint8_t index)
{
    const int64_t since = s_usb.rx_filled_us[index] > s_usb.rx_released_us ?
                              s_usb.rx_filled_us[index] :
                              s_usb.rx_released_us;
    const int64_t wait = esp_timer_get_time() - since;
    if (wait > (int64_t)s_usb.counters.read_wait_max_us) {
        s_usb.counters.read_wait_max_us = (uint32_t)wait;
    }
    if (wait > OSVC_USB_SLOW_READ_MS * 1000LL) {
        s_usb.counters.slow_reads++;
    }
}

bool osvc_usb_receive(const uint8_t **data, size_t *length)
{
    bool found = false;
    portENTER_CRITICAL(&s_usb.lock);
    if (s_usb.rx_paused) {
        drop_filled_locked();
    } else if (s_usb.rx_queue_count > 0U) {
        const uint8_t index = s_usb.rx_queue[0];
        for (size_t i = 1U; i < s_usb.rx_queue_count; ++i) {
            s_usb.rx_queue[i - 1U] = s_usb.rx_queue[i];
        }
        s_usb.rx_queue_count--;
        s_usb.rx_state[index] = RX_READING;
        s_usb.rx_reading_index = index;
        count_read_wait_locked(index);
        *data = s_usb.rx_buffer[index];
        *length = s_usb.rx_length[index];
        found = true;
    }
    portEXIT_CRITICAL(&s_usb.lock);
    return found;
}

void osvc_usb_receive_done(void)
{
    portENTER_CRITICAL(&s_usb.lock);
    if (s_usb.rx_state[s_usb.rx_reading_index] == RX_READING) {
        s_usb.rx_state[s_usb.rx_reading_index] = RX_FREE;
    }
    s_usb.rx_released_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_usb.lock);
    kick();
}

bool osvc_usb_write(const uint8_t *const parts[], const size_t lengths[],
                    size_t count, uint32_t timeout_ms)
{
    if (!s_usb.ready) {
        return false;
    }
    uint64_t total = 0U;
    for (size_t i = 0U; i < count; ++i) {
        total += lengths[i];
    }
    if (total > TX_RING_BYTES) {
        return false;
    }
    portENTER_CRITICAL(&s_usb.lock);
    const uint32_t reset_sequence = s_usb.reset_sequence;
    const uint32_t reconfigure_sequence = s_usb.reconfigure_sequence;
    portEXIT_CRITICAL(&s_usb.lock);
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (osvc_tx_ring_write(&s_usb.ring, parts, lengths, count)) {
            kick();
            return true;
        }
        kick();
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return false;
        }
        const int64_t wait_ms = remaining_us / 1000 + 1;
        xSemaphoreTake(s_usb.tx_space,
                       pdMS_TO_TICKS(wait_ms < 50 ? wait_ms : 50));
        portENTER_CRITICAL(&s_usb.lock);
        const bool interrupted =
            s_usb.reset_sequence != reset_sequence ||
            s_usb.reconfigure_sequence != reconfigure_sequence ||
            !s_usb.configured;
        portEXIT_CRITICAL(&s_usb.lock);
        if (interrupted) {
            return false;
        }
    }
}

int64_t osvc_usb_in_waiting_ms(void)
{
    portENTER_CRITICAL(&s_usb.lock);
    const int64_t started = s_usb.in_started_us;
    portEXIT_CRITICAL(&s_usb.lock);
    return started == 0 ? 0 : (esp_timer_get_time() - started) / 1000;
}

void osvc_usb_get_counters(osvc_usb_counters_t *counters)
{
    portENTER_CRITICAL(&s_usb.lock);
    *counters = s_usb.counters;
    portEXIT_CRITICAL(&s_usb.lock);
}
