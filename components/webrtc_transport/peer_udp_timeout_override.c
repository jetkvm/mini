/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "sdkconfig.h"

#include <errno.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_peer_udp_policy.h"

#include "webrtc_transport_private.h"

static const char *TAG = "webrtc_udp_poll";

typedef struct {
    TaskHandle_t owner;
    uint32_t remaining;
    uint32_t used;
    uint32_t max_used;
    uint64_t slices;
    uint64_t real_attempts;
    uint64_t quota_exhaustions;
    uint64_t empty_returns;
    uint64_t receive_errors;
    uint64_t begin_collisions;
    bool active;
    /* The slice refused a receive attempt: packets may wait in the socket. */
    bool refused;
    /* The attempts the slice had left when it stopped at a keyboard change. */
    uint32_t left_at_key;
} peer_rx_slice_policy_t;

static peer_rx_slice_policy_t s_peer_rx_slice;
static portMUX_TYPE s_peer_rx_slice_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_peer_rx_slice_last_report_us;

int webrtc_peer_udp_handshake_timeout_ms(void)
{
    /* esp_peer's dtls_recv() treats eleven empty receives as a closed
     * connection. A one-tick wait can lose a DTLS flight over DERP. Connected
     * peers use bounded non-blocking receive slices, so this wait only
     * affects setup. */
    return CONFIG_WEBRTC_TRANSPORT_AGENT_RECV_TIMEOUT_MS < 100 ?
        100 : CONFIG_WEBRTC_TRANSPORT_AGENT_RECV_TIMEOUT_MS;
}

/*
 * esp_peer exposes this function as a weak transport hook. lwIP's select()
 * timeout is ultimately rounded up by sys_arch_sem_wait().  esp_peer's DTLS
 * receive callback, dtls_recv() in third_party/esp_peer/src/core/
 * peer_default.c, treats eleven empty agent receives as
 * MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY, so converting the configured 1 ms wait
 * to a zero-time poll makes any delayed ClientHello look like a disconnect.
 * Clamp only the configured agent wait to one scheduler tick.  Other waits
 * selected internally by esp_peer (for example the 150 ms ICE check) retain
 * their requested duration.
 */
void udp_blocking_timeout(udp_socket_t *udp_socket, long long int ms)
{
    long long int effective_ms = ms;
    if (ms == CONFIG_WEBRTC_TRANSPORT_AGENT_RECV_TIMEOUT_MS &&
        ms > 0 && ms < portTICK_PERIOD_MS) {
        effective_ms = portTICK_PERIOD_MS;
    }

    udp_socket->timeout_sec = effective_ms / 1000;
    udp_socket->timeout_usec = (long)(effective_ms % 1000) * 1000;
}

void webrtc_peer_udp_rx_slice_begin(bool bounded, uint32_t max_attempts)
{
    if (!bounded) {
        return;
    }
    if (max_attempts == 0U) {
        max_attempts = 1U;
    }

    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    bool collision = false;
    portENTER_CRITICAL(&s_peer_rx_slice_lock);
    if (s_peer_rx_slice.active) {
        s_peer_rx_slice.begin_collisions++;
        collision = true;
    }
    s_peer_rx_slice.owner = current;
    s_peer_rx_slice.remaining = max_attempts;
    s_peer_rx_slice.used = 0U;
    s_peer_rx_slice.active = true;
    s_peer_rx_slice.refused = false;
    s_peer_rx_slice.left_at_key = 0U;
    s_peer_rx_slice.slices++;
    portEXIT_CRITICAL(&s_peer_rx_slice_lock);
    if (collision) {
        ESP_LOGE(TAG, "recovered nested peer UDP receive slice");
    }
}

/* This is the strong policy implementation for the weak hook in
 * third_party/esp_peer/src/transport/udp.c. */
bool udp_socket_recvfrom_poll_allowed(bool *nowait)
{
    if (nowait == NULL) {
        return false;
    }

    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    const bool keyboard_change = webrtc_transport_keyboard_change_pending();
    bool allowed = true;
    portENTER_CRITICAL(&s_peer_rx_slice_lock);
    if (s_peer_rx_slice.active && s_peer_rx_slice.owner == current) {
        /* A key that this slice read is on the host, and the browser waits
         * for the Mini's report of it, which the peer task sends after the
         * slice. Each further datagram of a file transfer would hold the
         * report back by 2.5 ms on the Wi-Fi board. The slice stops here
         * and keeps the count of the attempts it had left, which the peer
         * task reads after the report. A slice reads one datagram at
         * least, so input goes on while keys change all the time, as in a
         * paste. */
        if (keyboard_change && s_peer_rx_slice.used > 0U &&
            s_peer_rx_slice.remaining > 0U) {
            s_peer_rx_slice.left_at_key = s_peer_rx_slice.remaining;
            s_peer_rx_slice.remaining = 0U;
        }
        if (s_peer_rx_slice.remaining == 0U) {
            s_peer_rx_slice.quota_exhaustions++;
            s_peer_rx_slice.refused = true;
            allowed = false;
        } else {
            s_peer_rx_slice.remaining--;
            s_peer_rx_slice.used++;
            s_peer_rx_slice.real_attempts++;
            *nowait = true;
        }
    }
    portEXIT_CRITICAL(&s_peer_rx_slice_lock);
    return allowed;
}

/* esp_peer reads each STUN/TURN server reply once after sending the request,
 * five times in all. Inside a receive slice that read is a zero-time poll, so
 * the reply never arrives and a cloud peer gets no srflx candidate.
 * The ICE agent routes that server reply read here, which waits up
 * to 100 ms outside the slice. */
extern int udp_socket_recvfrom_nowait(udp_socket_t *, esp_peer_addr_t *,
                                      uint8_t *, int, bool);
int jetkvm_ice_server_recv(udp_socket_t *udp_socket, esp_peer_addr_t *addr,
                           uint8_t *buf, int len, bool nowait)
{
    (void)nowait;
    const long long int timeout_sec = udp_socket->timeout_sec;
    const long int timeout_usec = udp_socket->timeout_usec;
    portENTER_CRITICAL(&s_peer_rx_slice_lock);
    const bool slice_active = s_peer_rx_slice.active;
    s_peer_rx_slice.active = false;
    portEXIT_CRITICAL(&s_peer_rx_slice_lock);

    udp_blocking_timeout(udp_socket, 100);
    int ret = udp_socket_recvfrom_nowait(udp_socket, addr, buf, len, false);
    udp_socket->timeout_sec = timeout_sec;
    udp_socket->timeout_usec = timeout_usec;

    portENTER_CRITICAL(&s_peer_rx_slice_lock);
    s_peer_rx_slice.active = slice_active;
    portEXIT_CRITICAL(&s_peer_rx_slice_lock);
    return ret;
}

void udp_socket_recvfrom_poll_complete(int result)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_peer_rx_slice_lock);
    if (s_peer_rx_slice.active && s_peer_rx_slice.owner == current) {
        if (result == 0) {
            s_peer_rx_slice.empty_returns++;
        } else if (result < 0) {
            s_peer_rx_slice.receive_errors++;
        }
    }
    portEXIT_CRITICAL(&s_peer_rx_slice_lock);
}

/* The datagram that waits in the socket, with its sender: its length, 0
 * when none waits, -1 on an error. The results are those of esp_peer's
 * receive (udp_socket_recv_dispatch in udp.c). */
static int receive_waiting_datagram(int fd, esp_peer_addr_t *addr,
                                    uint8_t *buf, int len)
{
    struct sockaddr_storage from;
    socklen_t from_len = sizeof(from);
    const int ret = recvfrom(fd, buf, len, MSG_DONTWAIT,
                             (struct sockaddr *)&from, &from_len);
    if (ret < 0) {
        if (errno == EWOULDBLOCK) {
            return 0;
        }
        ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
        return -1;
    }
    if (ret == 0) {
        return -1;
    }
    if (from.ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)&from;
        addr->family = AF_INET;
        addr->port = ntohs(sin->sin_port);
        memcpy(addr->ipv4, &sin->sin_addr.s_addr, 4);
    } else if (from.ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&from;
        addr->family = AF_INET6;
        addr->port = ntohs(sin6->sin6_port);
        memcpy(addr->ipv6, &sin6->sin6_addr, 16);
    }
    return ret;
}

int __real_udp_socket_recvfrom_nowait(udp_socket_t *udp_socket,
                                      esp_peer_addr_t *addr, uint8_t *buf,
                                      int len, bool nowait);

/* esp_peer's receive asks select() whether a datagram waits and then reads
 * it, also when it does not wait for one. The peer task polls the socket for
 * every datagram of a receive slice and once more at the slice's end, 300
 * to 400 times per second during a file transfer, and on the P4 select()
 * takes 0.27 ms of such a poll's 0.38 ms. A poll reads the socket with
 * MSG_DONTWAIT instead. A receive that waits stays esp_peer's: outside a
 * slice the policy above counts nothing, so esp_peer's own call of it
 * changes nothing. */
int __wrap_udp_socket_recvfrom_nowait(udp_socket_t *udp_socket,
                                      esp_peer_addr_t *addr, uint8_t *buf,
                                      int len, bool nowait)
{
    bool poll = nowait;
    if (!udp_socket_recvfrom_poll_allowed(&poll)) {
        return 0;
    }
    if (!poll) {
        return __real_udp_socket_recvfrom_nowait(udp_socket, addr, buf, len,
                                                 false);
    }
    /* As esp_peer does: count the user before reading a descriptor that a
     * close may revoke, IPv4 first. */
    atomic_fetch_add(&udp_socket->user_count, 1);
    const int fd = __atomic_load_n(&udp_socket->fd, __ATOMIC_SEQ_CST);
    const int ipv6_fd = __atomic_load_n(&udp_socket->ipv6_fd, __ATOMIC_SEQ_CST);
    int ret = fd >= 0 ? receive_waiting_datagram(fd, addr, buf, len) :
              ipv6_fd >= 0 ? 0 : -1;
    if (ret == 0 && ipv6_fd >= 0) {
        ret = receive_waiting_datagram(ipv6_fd, addr, buf, len);
    }
    atomic_fetch_sub(&udp_socket->user_count, 1);
    udp_socket_recvfrom_poll_complete(ret);
    return ret;
}

bool webrtc_peer_udp_rx_slice_end(uint32_t *attempts_left)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    peer_rx_slice_policy_t snapshot = {0};
    bool report = false;
    bool refused = false;
    int64_t now_us = esp_timer_get_time();

    *attempts_left = 0U;
    portENTER_CRITICAL(&s_peer_rx_slice_lock);
    if (s_peer_rx_slice.active && s_peer_rx_slice.owner == current) {
        if (s_peer_rx_slice.used > s_peer_rx_slice.max_used) {
            s_peer_rx_slice.max_used = s_peer_rx_slice.used;
        }
        refused = s_peer_rx_slice.refused;
        *attempts_left = s_peer_rx_slice.left_at_key;
        s_peer_rx_slice.active = false;
        s_peer_rx_slice.owner = NULL;
        s_peer_rx_slice.remaining = 0U;
        s_peer_rx_slice.used = 0U;
        if (s_peer_rx_slice_last_report_us == 0 ||
            now_us - s_peer_rx_slice_last_report_us >= 2000000LL) {
            s_peer_rx_slice_last_report_us = now_us;
            snapshot = s_peer_rx_slice;
            report = true;
        }
    }
    portEXIT_CRITICAL(&s_peer_rx_slice_lock);

    if (report) {
        ESP_LOGI(TAG,
                 "peer rx slices=%" PRIu64 " attempts=%" PRIu64
                 " quota=%" PRIu64 " empty=%" PRIu64
                 " errors=%" PRIu64 " max_used=%" PRIu32
                 " collisions=%" PRIu64,
                 snapshot.slices, snapshot.real_attempts,
                 snapshot.quota_exhaustions, snapshot.empty_returns,
                 snapshot.receive_errors, snapshot.max_used,
                 snapshot.begin_collisions);
    }
    return refused;
}

void webrtc_peer_udp_timeout_override_init(void)
{
    static bool reported;
    if (reported) {
        return;
    }
    reported = true;

    const int timeout_ms = webrtc_peer_udp_handshake_timeout_ms();
    const int effective_ms = timeout_ms > 0 &&
                                     timeout_ms < portTICK_PERIOD_MS ?
                                 (int)portTICK_PERIOD_MS : timeout_ms;
    ESP_LOGI(TAG,
             "agent receive timeout: requested=%d ms tick=%d ms effective=%d ms",
             timeout_ms, (int)portTICK_PERIOD_MS, effective_ms);
}
