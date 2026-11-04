#!/usr/bin/env python3
"""Run the peer task's receive slice policy and its poll of the socket with host stubs."""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / "peer_udp_timeout_override.c").read_text()


def block(text, start_text, opening="{", closing="}"):
    start = text.index(start_text)
    position = text.index(opening, start) + 1
    depth = 1
    while depth:
        depth += (text[position] == opening) - (text[position] == closing)
        position += 1
    return text[start:position]


policy = block(source, "typedef struct {\n    TaskHandle_t owner;") + " peer_rx_slice_policy_t;\n"
functions = "\n".join(block(source, signature) for signature in (
    "void webrtc_peer_udp_rx_slice_begin(",
    "bool udp_socket_recvfrom_poll_allowed(",
    "void udp_socket_recvfrom_poll_complete(",
    "static int receive_waiting_datagram(",
    "int __wrap_udp_socket_recvfrom_nowait(",
    "bool webrtc_peer_udp_rx_slice_end(",
))

stub = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef struct { int family; uint16_t port; union { uint8_t ipv4[4]; uint8_t ipv6[16]; }; } esp_peer_addr_t;
typedef struct { int fd, ipv6_fd; atomic_int user_count; } udp_socket_t;
/* The sockets: what the next recvfrom of each descriptor returns. */
enum { FD4 = 5, FD6 = 6 };
static int waiting[8], receive_errno[8], receive_calls, real_calls;
static ssize_t test_recvfrom(int fd, void *buf, size_t len, int flags,
                             struct sockaddr *from, socklen_t *from_len)
{
    (void)buf; (void)len;
    receive_calls++;
    assert(flags == MSG_DONTWAIT);
    if (receive_errno[fd]) { errno = receive_errno[fd]; return -1; }
    if (!waiting[fd]) { errno = EWOULDBLOCK; return -1; }
    if (fd == FD4) {
        struct sockaddr_in *sin = (struct sockaddr_in *)from;
        sin->sin_family = AF_INET; sin->sin_port = htons(4000);
        sin->sin_addr.s_addr = htonl(0xc0000201);
        *from_len = sizeof(*sin);
    } else {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)from;
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6; sin6->sin6_port = htons(6000);
        sin6->sin6_addr.s6_addr[15] = 9;
        *from_len = sizeof(*sin6);
    }
    const int length = waiting[fd];
    waiting[fd] = 0;
    return length;
}
#define recvfrom test_recvfrom
static int __real_udp_socket_recvfrom_nowait(udp_socket_t *s, esp_peer_addr_t *a, uint8_t *b, int l, bool nowait)
{
    (void)s; (void)a; (void)b; (void)l;
    assert(!nowait);
    real_calls++;
    return 77;
}
typedef int *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static void log_stub(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGE log_stub
#define ESP_LOGI log_stub
static const char *TAG = "test";
static int peer_task, other_task;
static TaskHandle_t current_task = &peer_task;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
static int64_t esp_timer_get_time(void) { return 1; }
static bool keyboard_change;
static bool webrtc_transport_keyboard_change_pending(void) { return keyboard_change; }
'''

checks = r'''
static peer_rx_slice_policy_t s_peer_rx_slice;
static portMUX_TYPE s_peer_rx_slice_lock;
static int64_t s_peer_rx_slice_last_report_us;
''' + functions + r'''
/* One receive attempt of the vendor loop: whether the policy let it read. */
static bool attempt(void)
{
    bool nowait = false;
    if (!udp_socket_recvfrom_poll_allowed(&nowait)) return false;
    assert(nowait);
    udp_socket_recvfrom_poll_complete(1200);
    return true;
}

/* Ends the slice: whether it refused an attempt. *left gets the attempts it
 * had left when it stopped at a keyboard change. */
static bool end(uint32_t *left)
{
    *left = 99U;
    return webrtc_peer_udp_rx_slice_end(left);
}

int main(void)
{
    (void)TAG; (void)other_task;
    uint32_t left;
    /* A slice reads its 8 datagrams and refuses the ninth. */
    webrtc_peer_udp_rx_slice_begin(true, 8U);
    for (int i = 0; i < 8; ++i) assert(attempt());
    assert(!attempt());
    assert(end(&left) && left == 0U);

    /* A slice that the socket's end stopped refused nothing. */
    webrtc_peer_udp_rx_slice_begin(true, 8U);
    assert(attempt() && attempt());
    assert(!end(&left) && left == 0U);

    /* The third datagram is a key: its change on the host stops the slice,
     * which has five attempts left. */
    webrtc_peer_udp_rx_slice_begin(true, 8U);
    assert(attempt() && attempt() && attempt());
    keyboard_change = true;
    assert(!attempt() && !attempt());
    assert(end(&left) && left == 5U);

    /* While the change waits, the slice of the rest still reads one
     * datagram: input does not stop when the keys change faster than the
     * task sends them. Four attempts are left. */
    webrtc_peer_udp_rx_slice_begin(true, 5U);
    assert(attempt());
    assert(!attempt());
    assert(end(&left) && left == 4U);

    /* Sent: the slice of the rest reads to its limit, and nothing is left. */
    keyboard_change = false;
    webrtc_peer_udp_rx_slice_begin(true, 4U);
    for (int i = 0; i < 4; ++i) assert(attempt());
    assert(!attempt());
    assert(end(&left) && left == 0U);

    /* A change that comes with a slice's last attempt leaves nothing. */
    webrtc_peer_udp_rx_slice_begin(true, 2U);
    assert(attempt() && attempt());
    keyboard_change = true;
    assert(!attempt());
    assert(end(&left) && left == 0U);

    /* Another task's receive is outside the slice and not limited. */
    webrtc_peer_udp_rx_slice_begin(true, 1U);
    assert(attempt());
    current_task = &other_task;
    bool nowait = false;
    assert(udp_socket_recvfrom_poll_allowed(&nowait) && !nowait);
    current_task = &peer_task;
    assert(!end(&left) && left == 0U);

    /* A poll inside a slice reads the socket without esp_peer's select():
     * the IPv4 datagram with its sender, then nothing. */
    udp_socket_t sockets = {.fd = FD4, .ipv6_fd = FD6};
    esp_peer_addr_t from = {0};
    uint8_t packet[1500];
    keyboard_change = false;
    webrtc_peer_udp_rx_slice_begin(true, 4U);
    waiting[FD4] = 1200;
    assert(__wrap_udp_socket_recvfrom_nowait(&sockets, &from, packet, sizeof(packet), false) == 1200);
    assert(from.family == AF_INET && from.port == 4000 && from.ipv4[0] == 192 && from.ipv4[3] == 1);
    assert(__wrap_udp_socket_recvfrom_nowait(&sockets, &from, packet, sizeof(packet), false) == 0);
    assert(s_peer_rx_slice.empty_returns == 1);
    /* The IPv6 socket is read when the IPv4 one has nothing. */
    waiting[FD6] = 60;
    assert(__wrap_udp_socket_recvfrom_nowait(&sockets, &from, packet, sizeof(packet), false) == 60);
    assert(from.family == AF_INET6 && from.port == 6000 && from.ipv6[15] == 9);
    /* A failed read is an error, and the slice counts it. */
    receive_errno[FD4] = EBADF;
    assert(__wrap_udp_socket_recvfrom_nowait(&sockets, &from, packet, sizeof(packet), false) == -1);
    assert(s_peer_rx_slice.receive_errors == 1);
    receive_errno[FD4] = 0;
    /* The slice's four attempts are used: the fifth reads nothing. */
    const int calls = receive_calls;
    waiting[FD4] = 1200;
    assert(__wrap_udp_socket_recvfrom_nowait(&sockets, &from, packet, sizeof(packet), false) == 0);
    assert(receive_calls == calls && waiting[FD4] == 1200);
    assert(end(&left) && left == 0U);
    /* Outside a slice its end has nothing left to report. */
    assert(!end(&left) && left == 0U);
    /* Outside a slice a receive that waits is esp_peer's, and a poll still
     * reads directly. */
    assert(__wrap_udp_socket_recvfrom_nowait(&sockets, &from, packet, sizeof(packet), false) == 77);
    assert(real_calls == 1 && receive_calls == calls);
    assert(__wrap_udp_socket_recvfrom_nowait(&sockets, &from, packet, sizeof(packet), true) == 1200);
    assert(real_calls == 1);
    /* Closed sockets are an error, and no call leaves a user counted. */
    sockets.fd = sockets.ipv6_fd = -1;
    assert(__wrap_udp_socket_recvfrom_nowait(&sockets, &from, packet, sizeof(packet), true) == -1);
    assert(atomic_load(&sockets.user_count) == 0);
    puts("peer receive slice: limit, stop at a keyboard change with the attempts left, one datagram at least, poll without select: OK");
}
'''
with tempfile.TemporaryDirectory(prefix="mini-rx-slice-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(stub + policy + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
