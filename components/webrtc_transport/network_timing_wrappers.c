/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_eth_driver.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "srtp.h"
#include "cipher.h"

#include "network_timing.h"
#include "webrtc_transport_private.h"

void webrtc_network_timing_wrappers_link_anchor(void)
{
}

srtp_err_status_t __real_srtp_unprotect_rtcp(
    srtp_t ctx, const uint8_t *srtcp, size_t srtcp_len, uint8_t *rtcp,
    size_t *rtcp_len);

srtp_err_status_t __wrap_srtp_unprotect_rtcp(
    srtp_t ctx, const uint8_t *srtcp, size_t srtcp_len, uint8_t *rtcp,
    size_t *rtcp_len)
{
    srtp_err_status_t result = __real_srtp_unprotect_rtcp(
        ctx, srtcp, srtcp_len, rtcp, rtcp_len);
    if (result == srtp_err_status_ok && rtcp && rtcp_len) {
        webrtc_transport_on_rtcp(rtcp, *rtcp_len);
    }
    return result;
}

srtp_err_status_t __real_srtp_protect(
    srtp_t ctx, const uint8_t *rtp, size_t rtp_len, uint8_t *srtp,
    size_t *srtp_len, unsigned int mki_index);

static TaskHandle_t s_srtp_measurement_owner;
static uint32_t s_srtp_cipher_us;

srtp_err_status_t __real_srtp_cipher_encrypt(
    srtp_cipher_t *cipher, const uint8_t *src, size_t src_len, uint8_t *dst,
    size_t *dst_len);

srtp_err_status_t __wrap_srtp_cipher_encrypt(
    srtp_cipher_t *cipher, const uint8_t *src, size_t src_len, uint8_t *dst,
    size_t *dst_len)
{
    int64_t started_us = esp_timer_get_time();
    srtp_err_status_t result =
        __real_srtp_cipher_encrypt(cipher, src, src_len, dst, dst_len);
    int64_t completed_us = esp_timer_get_time();
    if (s_srtp_measurement_owner == xTaskGetCurrentTaskHandle() &&
        completed_us >= started_us) {
        uint64_t elapsed = (uint64_t)(completed_us - started_us);
        if (elapsed <= UINT32_MAX - s_srtp_cipher_us) {
            s_srtp_cipher_us += (uint32_t)elapsed;
        } else {
            s_srtp_cipher_us = UINT32_MAX;
        }
    }
    return result;
}

srtp_err_status_t __wrap_srtp_protect(
    srtp_t ctx, const uint8_t *rtp, size_t rtp_len, uint8_t *srtp,
    size_t *srtp_len, unsigned int mki_index)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    bool measure_breakdown = s_srtp_measurement_owner == NULL;
    if (measure_breakdown) {
        s_srtp_measurement_owner = current_task;
        s_srtp_cipher_us = 0;
    }
    const size_t output_capacity = srtp_len != NULL ? *srtp_len : 0;
    int64_t started_us = esp_timer_get_time();
    srtp_err_status_t result = __real_srtp_protect(
        ctx, rtp, rtp_len, srtp, srtp_len, mki_index);
    int64_t completed_us = esp_timer_get_time();
    webrtc_network_timing_record_rtp(
        WEBRTC_NETWORK_STAGE_SRTP, rtp, rtp_len, started_us, completed_us,
        result == srtp_err_status_ok);
    if (result != srtp_err_status_ok) {
        webrtc_network_timing_record_srtp_error(
            (int)result, rtp, rtp_len, output_capacity,
            srtp_len != NULL ? *srtp_len : 0);
    }
    if (measure_breakdown) {
        uint64_t total_us = completed_us >= started_us ?
            (uint64_t)(completed_us - started_us) : 0;
        uint32_t bounded_total = total_us > UINT32_MAX ?
            UINT32_MAX : (uint32_t)total_us;
        uint32_t other_us = bounded_total >= s_srtp_cipher_us ?
            bounded_total - s_srtp_cipher_us : 0;
        webrtc_network_timing_record_srtp_breakdown(
            rtp, rtp_len, s_srtp_cipher_us, other_us);
        s_srtp_measurement_owner = NULL;
    }
    return result;
}

ssize_t __real_lwip_sendto(int socket, const void *data, size_t size,
                           int flags, const struct sockaddr *to,
                           socklen_t tolen);

ssize_t __wrap_lwip_sendto(int socket, const void *data, size_t size,
                           int flags, const struct sockaddr *to,
                           socklen_t tolen)
{
    int64_t started_us = esp_timer_get_time();
    ssize_t result = __real_lwip_sendto(socket, data, size, flags, to, tolen);
    int64_t completed_us = esp_timer_get_time();
    webrtc_network_timing_record_rtp(
        WEBRTC_NETWORK_STAGE_LWIP_SENDTO, data, size, started_us, completed_us,
        result >= 0);
    return result;
}

struct pbuf;
struct netif;
void __real_udp_input(struct pbuf *p, struct netif *inp);

/* lwIP's input of every UDP datagram, on the TCP/IP task. The peer task
 * sleeps for up to one tick (10 ms) between two passes. The datagram ends
 * that sleep, as a frame and a keyboard change do, so that it does not wait
 * in the socket for the rest of the tick: a key as well as a file
 * transfer's data. A datagram of another socket (DNS, mDNS, DHCP) costs the
 * peer task one pass without work. */
void __wrap_udp_input(struct pbuf *p, struct netif *inp)
{
    __real_udp_input(p, inp);
    webrtc_transport_datagram_arrived();
}

esp_err_t __real_esp_eth_transmit(esp_eth_handle_t hdl, void *buffer,
                                  size_t length);

esp_err_t __wrap_esp_eth_transmit(esp_eth_handle_t hdl, void *buffer,
                                  size_t length)
{
    int64_t started_us = esp_timer_get_time();
    esp_err_t result = __real_esp_eth_transmit(hdl, buffer, length);
    int64_t completed_us = esp_timer_get_time();
    webrtc_network_timing_record_ethernet(
        buffer, length, started_us, completed_us, result == ESP_OK);
    return result;
}
