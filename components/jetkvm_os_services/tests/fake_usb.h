/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* Stand-ins for the TinyUSB, cache and jetkvm_usb_hid calls of
 * os_services_usb.c, on top of fake_idf.h. run_host_tests.sh serves every
 * such header that file includes from this one; the test defines the
 * TinyUSB calls. */

#pragma once

#include <stdbool.h>

#include "fake_idf.h"

#define CONFIG_JETKVM_OS_SERVICES_USB_BUFFER_KIB 16

#define MALLOC_CAP_DMA 4U
static inline void *heap_caps_aligned_alloc(size_t alignment, size_t size,
                                            uint32_t caps)
{
    return aligned_alloc(alignment, size);
}
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 2
static inline esp_err_t esp_cache_get_alignment(uint32_t caps,
                                                size_t *alignment)
{
    *alignment = 64U;
    return ESP_OK;
}
static inline esp_err_t esp_cache_msync(void *address, size_t size, int flags)
{
    return ESP_OK;
}

typedef enum {
    XFER_RESULT_SUCCESS,
    XFER_RESULT_FAILED,
    XFER_RESULT_STALLED,
} xfer_result_t;
enum { CONTROL_STAGE_SETUP = 1 };
enum { TUSB_DESC_ENDPOINT = 5 };
enum { TUSB_XFER_BULK = 2 };
enum { TUSB_DIR_OUT = 0, TUSB_DIR_IN = 1 };
enum { TUSB_SPEED_FULL = 0, TUSB_SPEED_HIGH = 2 };

/* Packed, as on the wire. */
typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} tusb_desc_interface_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    struct {
        uint8_t xfer;
    } bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} tusb_desc_endpoint_t;

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} tusb_control_request_t;

static inline const uint8_t *tu_desc_next(const void *descriptor)
{
    const uint8_t *bytes = descriptor;
    return bytes + bytes[0];
}
static inline uint8_t tu_desc_type(const void *descriptor)
{
    return ((const uint8_t *)descriptor)[1];
}
static inline int tu_edpt_dir(uint8_t address)
{
    return (address & 0x80U) != 0U ? TUSB_DIR_IN : TUSB_DIR_OUT;
}
static inline uint16_t tu_edpt_packet_size(const tusb_desc_endpoint_t *ep)
{
    return ep->wMaxPacketSize & 0x7ffU;
}

typedef struct {
    const char *name;
    void (*init)(void);
    bool (*deinit)(void);
    void (*reset)(uint8_t rhport);
    uint16_t (*open)(uint8_t rhport, const tusb_desc_interface_t *itf,
                     uint16_t max_length);
    bool (*control_xfer_cb)(uint8_t rhport, uint8_t stage,
                            const tusb_control_request_t *request);
    bool (*xfer_cb)(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                    uint32_t transferred);
    void *xfer_isr;
    void *sof;
} usbd_class_driver_t;

bool usbd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *endpoint);
bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer,
                    uint16_t total_bytes);
int tud_speed_get(void);
bool tud_control_xfer(uint8_t rhport, const tusb_control_request_t *request,
                      void *buffer, uint16_t length);
void usbd_defer_func(void (*func)(void *), void *param, bool in_isr);

typedef struct {
    bool (*vendor_request)(uint8_t rhport, uint8_t stage, const void *request,
                           void *ctx);
    void (*stack_stopping)(void *ctx);
    void *ctx;
} jetkvm_usb_hid_os_services_hooks_t;
esp_err_t jetkvm_usb_hid_set_os_services_hooks(
    const jetkvm_usb_hid_os_services_hooks_t *hooks);
