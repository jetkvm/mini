/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* The class driver's transfer completions (os_services_usb.c), with TinyUSB
 * faked by fake_usb.h and its task's calls made on this thread. */

#include <assert.h>
#include <stdio.h>

#include "../os_services_usb.c"

#define EP_OUT 0x01U
#define EP_IN 0x81U

static jetkvm_usb_hid_os_services_hooks_t s_hooks;
static uint8_t s_last_endpoint;

void xTaskNotifyGive(TaskHandle_t task) {}

esp_err_t jetkvm_usb_hid_set_os_services_hooks(
    const jetkvm_usb_hid_os_services_hooks_t *hooks)
{
    s_hooks = *hooks;
    return ESP_OK;
}

bool usbd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *endpoint)
{
    return true;
}

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer,
                    uint16_t total_bytes)
{
    s_last_endpoint = ep_addr;
    return true;
}

int tud_speed_get(void)
{
    return TUSB_SPEED_HIGH;
}

bool tud_control_xfer(uint8_t rhport, const tusb_control_request_t *request,
                      void *buffer, uint16_t length)
{
    return true;
}

void usbd_defer_func(void (*func)(void *), void *param, bool in_isr)
{
    func(param);
}

static const usbd_class_driver_t *driver(void)
{
    uint8_t count = 0U;
    return usbd_app_driver_get_cb(&count);
}

/* TinyUSB starts, the host configures the interface and sends RESET, and
 * the service task starts the transport for that session. */
static void init_once(void)
{
    static bool initialized;
    if (!initialized) {
        assert(osvc_usb_init((TaskHandle_t)&s_hooks) == ESP_OK);
        driver()->init();
        initialized = true;
    }
}

static void open_session(void)
{
    init_once();
    struct __attribute__((packed)) {
        tusb_desc_interface_t itf;
        tusb_desc_endpoint_t out;
        tusb_desc_endpoint_t in;
    } descriptors = {
        .itf = {sizeof(tusb_desc_interface_t), 4U, 0U, 0U, 2U,
                INTERFACE_CLASS, INTERFACE_SUBCLASS, INTERFACE_PROTOCOL, 0U},
        .out = {sizeof(tusb_desc_endpoint_t), TUSB_DESC_ENDPOINT, EP_OUT,
                {TUSB_XFER_BULK}, 512U, 0U},
        .in = {sizeof(tusb_desc_endpoint_t), TUSB_DESC_ENDPOINT, EP_IN,
               {TUSB_XFER_BULK}, 512U, 0U},
    };
    driver()->reset(0U);
    assert(driver()->open(0U, &descriptors.itf, sizeof(descriptors)) ==
           sizeof(descriptors));
    const tusb_control_request_t reset = {REQUEST_TYPE, REQUEST_RESET,
                                          PROTOCOL_MAJOR, 0U,
                                          OSVC_USB_STATUS_LEN};
    assert(s_hooks.vendor_request(0U, CONTROL_STAGE_SETUP, &reset, NULL));
    osvc_usb_restart();
}

/* What the service task checks before it admits or keeps a session. */
static bool session_admitted(void)
{
    osvc_usb_events_t events;
    osvc_usb_get_events(&events);
    return events.configured &&
           events.reset_reconfigure_sequence == events.reconfigure_sequence;
}

static uint32_t reconfigure_sequence(void)
{
    osvc_usb_events_t events;
    osvc_usb_get_events(&events);
    return events.reconfigure_sequence;
}

static void queue_in_message(void)
{
    static const uint8_t message[100];
    const uint8_t *const parts[] = {message};
    const size_t lengths[] = {sizeof(message)};
    assert(osvc_usb_write(parts, lengths, 1U, 0U));
    assert(s_last_endpoint == EP_IN);
}

static void test_completed_transfers_keep_the_session(void)
{
    open_session();
    const uint32_t before = reconfigure_sequence();
    static const uint8_t short_read[16];
    memcpy(s_usb.rx_buffer[s_usb.rx_armed_index], short_read,
           sizeof(short_read));
    assert(driver()->xfer_cb(0U, EP_OUT, XFER_RESULT_SUCCESS,
                             sizeof(short_read)));
    queue_in_message();
    assert(driver()->xfer_cb(0U, EP_IN, XFER_RESULT_SUCCESS, 100U));
    assert(reconfigure_sequence() == before);
    assert(session_admitted());
}

/* The bytes of a failed read are gone, so the session's stream cannot go
 * on: the session ends and the sender must send RESET. */
static void test_failed_out_read_ends_the_session(void)
{
    open_session();
    assert(session_admitted());
    const uint32_t before = reconfigure_sequence();
    assert(s_last_endpoint == EP_OUT);
    assert(driver()->xfer_cb(0U, EP_OUT, XFER_RESULT_FAILED, 0U));
    assert(reconfigure_sequence() != before);
    assert(!session_admitted());
}

/* The host may have read part of a failed IN transfer, so neither sending
 * it again nor skipping it keeps the stream whole. */
static void test_failed_in_transfer_ends_the_session(void)
{
    open_session();
    queue_in_message();
    assert(session_admitted());
    const uint32_t before = reconfigure_sequence();
    assert(driver()->xfer_cb(0U, EP_IN, XFER_RESULT_FAILED, 0U));
    assert(reconfigure_sequence() != before);
    assert(!session_admitted());
}

static uint32_t restart_lag(void)
{
    osvc_usb_events_t events;
    osvc_usb_get_events(&events);
    return events.reconfigure_sequence - events.restart_reconfigure_sequence;
}

/* A fault recovery's TinyUSB restart counts against the video retry
 * budget: it is not the KVM's restart for a mount or a USB setting. */
static void test_recovery_restart_is_not_the_kvms(void)
{
    open_session();
    jetkvm_os_services_usb_recovery_next();
    s_hooks.stack_stopping(NULL);
    jetkvm_os_services_usb_recovery_done();
    driver()->init();
    assert(restart_lag() != 0U);
}

/* A recovery whose restart never stopped TinyUSB (busy, or USB off) leaves
 * the next restart, for a mount, the KVM's. */
static void test_recovery_without_restart_leaves_no_mark(void)
{
    open_session();
    jetkvm_os_services_usb_recovery_next();
    jetkvm_os_services_usb_recovery_done();
    s_hooks.stack_stopping(NULL);
    driver()->init();
    assert(restart_lag() == 0U);
}

/* A STATUS before the first RESET has a non-zero token, because the
 * protocol has no all-zero token. */
static void test_status_before_reset_has_a_token(void)
{
    init_once();
    static const uint8_t zero[OSVC_TOKEN_LEN];
    assert(memcmp(s_usb.status, "JKV5", 4U) == 0);
    assert(memcmp(s_usb.status + 8, zero, OSVC_TOKEN_LEN) != 0);
}

int main(void)
{
    test_status_before_reset_has_a_token();
    test_completed_transfers_keep_the_session();
    test_failed_out_read_ends_the_session();
    test_failed_in_transfer_ends_the_session();
    test_recovery_restart_is_not_the_kvms();
    test_recovery_without_restart_leaves_no_mark();
    puts("os_services_usb tests passed");
    return 0;
}
