#!/usr/bin/env python3
"""What the source-built peer's ICE agent does with a malformed STUN datagram.

The ICE socket takes datagrams from each sender before a credential is
checked. The receive function, the STUN parser and the STUN validator are
taken from the source text and compiled for this machine with the address
sanitizer, with an agent that has only the fields they use.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.tests.test_esp_peer_ice_pairing import typedef
from tools.tests.test_esp_peer_source_transport import function

ROOT = Path(__file__).resolve().parents[2]
PEER = ROOT / 'third_party/esp_peer'
CORE = PEER / 'src/core'

PRELUDE = r'''
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
/* The layout checks of the header are for the 32-bit target. */
#define _Static_assert(...) struct layout_check_of_the_target
#include "stun.h"
#include "agent.h"
#undef _Static_assert
#define PRIVATE static
#define LOGE(...) ((void)0)
'''

HARNESS = r'''
typedef struct { int fd; } udp_socket_t;
typedef struct agent_t agent_t;
struct agent_t {
    udp_socket_t stun_sock;
    pthread_mutex_t recv_mutex;
    ice_candidate_pair_t *selected_pair;
    bool tcp_turn_connected, use_stun_relay;
    void *tcp, *tls_turn;
    agent_mode_t mode;
    uint16_t stun_channel_num;
    ice_candidate_state_t stun_state;
    uint32_t last_peer_binding_time;
    int binding_not_response;
};
static bool allow_dump_recv;

/* The datagram that is in the socket. As recvfrom does, the socket gives
 * no more of it than the caller's buffer holds. */
static uint8_t datagram[1500];
static int datagram_size;
static int udp_socket_recvfrom_nowait(udp_socket_t *sock, esp_peer_addr_t *addr, uint8_t *buf, int len, bool no_wait)
{
    (void)sock; (void)no_wait;
    memset(addr, 0, sizeof(*addr));
    int size = datagram_size < len ? datagram_size : len;
    memcpy(buf, datagram, (size_t)size);
    return size;
}

/* What the agent gave to its STUN handlers. */
static int requests, responses;
static void agent_process_stun_request(agent_t *agent, esp_peer_addr_t *remote, stun_msg_t *msg)
{
    (void)agent; (void)remote; (void)msg; ++requests;
}
static void agent_process_stun_response(agent_t *agent, esp_peer_addr_t *remote, stun_msg_t *msg)
{
    (void)agent; (void)remote; (void)msg; ++responses;
}

static bool agent_turn_stream_is_tls(agent_t *agent) { (void)agent; return false; }
static int agent_recv_tcp_packet(agent_t *agent, esp_peer_addr_t *addr, uint8_t *buf, int len, bool no_wait)
{
    (void)agent; (void)addr; (void)buf; (void)len; (void)no_wait; return -1;
}
static void dump_recv_data(esp_peer_addr_t *addr, uint8_t *buf, int len) { (void)addr; (void)buf; (void)len; }
static uint32_t get_cur_time(void) { return 0; }
static int agent_update_credential(agent_t *agent, stun_msg_t *msg) { (void)agent; (void)msg; return -1; }
static int agent_send_fresh(agent_t *agent, int life_time) { (void)agent; (void)life_time; return 0; }
static int agent_send_permission(agent_t *agent, ice_candidate_t *remote, bool refresh)
{
    (void)agent; (void)remote; (void)refresh; return 0;
}
void *jetkvm_ice_find_pair(void *agent, const esp_peer_addr_t *source) { (void)agent; (void)source; return NULL; }
static void utils_get_md5(const char *in, size_t size, unsigned char *out) { (void)in; (void)size; memset(out, 0, 16); }
static void utils_get_hmac_sha1(const char *in, size_t size, const char *key, size_t key_size, unsigned char *out)
{
    (void)in; (void)size; (void)key; (void)key_size; memset(out, 0, 20);
}
void stun_calculate_fingerprint(char *buf, size_t len, uint32_t *fingerprint) { (void)buf; (void)len; *fingerprint = 0; }
'''

TESTS = r'''
enum { DATA_INDICATION = 0x0017, BINDING_REQUEST = 0x0001 };

static void put16(uint8_t *at, uint16_t value) { at[0] = (uint8_t)(value >> 8); at[1] = (uint8_t)value; }

/* Starts a datagram with a STUN header whose length field says that
 * `attributes` bytes follow. */
static void stun_datagram(uint16_t type, uint16_t attributes, int size)
{
    memset(datagram, 0, sizeof(datagram));
    put16(datagram, type); put16(datagram + 2, attributes);
    datagram[4] = 0x21; datagram[5] = 0x12; datagram[6] = 0xA4; datagram[7] = 0x42;
    datagram_size = size;
}

/* Receives the datagram into a heap buffer of this size, so that the
 * sanitizer stops the test at the first byte after it. */
static int receive(int capacity, uint8_t **received, bool *is_stun)
{
    static agent_t agent = {.stun_sock = {.fd = 3}, .recv_mutex = PTHREAD_MUTEX_INITIALIZER,
                            .mode = AGENT_MODE_CONTROLLED};
    free(*received);
    *received = malloc((size_t)capacity);
    requests = responses = 0;
    return agent_recv_one_packet(&agent, *received, capacity, true, is_stun);
}

int main(void)
{
    uint8_t *buf = NULL;
    bool is_stun;

    /* A Data indication of a TURN server with a payload that is not STUN:
     * the caller gets the payload. */
    stun_datagram(DATA_INDICATION, 4 + 100, 20 + 4 + 100);
    put16(datagram + 20, STUN_ATTR_TYPE_DATA); put16(datagram + 22, 100);
    memset(datagram + 24, 0x5A, 100);
    assert(receive(1300, &buf, &is_stun) == 100 && !is_stun);
    for (int i = 0; i < 100; ++i) assert(buf[i] == 0x5A);

    /* A Data indication whose DATA attribute says that it has more bytes
     * than the datagram: the agent copies nothing, the caller gets no
     * payload, and the message has no handler. */
    stun_datagram(DATA_INDICATION, 4, 32);
    put16(datagram + 20, STUN_ATTR_TYPE_DATA); put16(datagram + 22, 1500);
    assert(receive(1300, &buf, &is_stun) == 32 && is_stun);
    assert(memcmp(buf, datagram, 32) == 0);
    assert(requests == 0 && responses == 0);
    /* The same with the largest length, and with a header that says that
     * the attribute fits. */
    stun_datagram(DATA_INDICATION, 0xFFFF, 32);
    put16(datagram + 20, STUN_ATTR_TYPE_DATA); put16(datagram + 22, 0xFFFF);
    assert(receive(1300, &buf, &is_stun) == 32 && is_stun);
    assert(requests == 0 && responses == 0);

    /* The DTLS handshake reads with a buffer that is larger than a STUN
     * message. A datagram that fills it is used up and has no handler. */
    stun_datagram(BINDING_REQUEST, 0, 1472);
    memset(datagram + 20, 0xEE, 1452);
    assert(receive(1500, &buf, &is_stun) == 1472 && is_stun);
    assert(requests == 0 && responses == 0);
    /* The largest STUN message has a handler. */
    stun_datagram(BINDING_REQUEST, 0, (int)sizeof(((stun_msg_t *)0)->buf));
    assert(receive(1500, &buf, &is_stun) == 1400 && is_stun && requests == 1);
    free(buf);

    /* The parser reads the received bytes only. Here the bytes after them
     * are of an earlier message, and the header says that they belong to
     * this one. */
    stun_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    stun_datagram(BINDING_REQUEST, 100, 20);
    memcpy(msg.buf, datagram, 20);
    put16(msg.buf + 20, STUN_ATTR_TYPE_USERNAME); put16(msg.buf + 22, 8);
    put16(msg.buf + 32, STUN_ATTR_TYPE_USE_CANDIDATE);
    msg.size = 20;
    assert(stun_parse_msg_buf(&msg) == 0);
    assert(msg.username_size == 0 && !msg.use_candidate);
    /* With the bytes received, the parser has both attributes. */
    msg.size = 36;
    assert(stun_parse_msg_buf(&msg) == 0);
    assert(msg.username_size == 8 && msg.username == (char *)msg.buf + 24 && msg.use_candidate);

    /* The parser refuses an address attribute without its value, and reads
     * no address from the bytes after it. */
    memset(&msg, 0, sizeof(msg));
    stun_datagram(BINDING_REQUEST, 4, 24);
    memcpy(msg.buf, datagram, 24);
    put16(msg.buf + 20, STUN_ATTR_TYPE_MAPPED_ADDRESS);
    memset(msg.buf + 24, 0x11, 8);
    msg.size = 24;
    assert(stun_parse_msg_buf(&msg) == -1);
    assert(msg.mapped_addr.port == 0);
    /* With its value, it gives the address. */
    put16(msg.buf + 2, 12); put16(msg.buf + 22, 8);
    msg.size = 32;
    assert(stun_parse_msg_buf(&msg) == 0);
    assert(msg.mapped_addr.port == 0x1111 && msg.mapped_addr.family == AF_INET);

    /* A refused message leaves no attribute for the caller. Here a relayed
     * address comes before an attribute that does not fit. */
    memset(&msg, 0, sizeof(msg));
    stun_datagram(0x0103, 12 + 4, 36);
    memcpy(msg.buf, datagram, 36);
    put16(msg.buf + 20, STUN_ATTR_TYPE_XOR_RELAYED_ADDRESS); put16(msg.buf + 22, 8);
    memset(msg.buf + 24, 0x11, 8);
    put16(msg.buf + 32, STUN_ATTR_TYPE_DATA); put16(msg.buf + 34, 100);
    msg.size = 36;
    assert(stun_parse_msg_buf(&msg) == -1);
    assert(msg.relayed_addr.port == 0 && msg.stunmethod == 0);
    /* Without the attribute that does not fit, the caller gets the address. */
    put16(msg.buf + 2, 12);
    msg.size = 32;
    assert(stun_parse_msg_buf(&msg) == 0);
    assert(msg.relayed_addr.port != 0 && msg.stunmethod == STUN_METHOD_ALLOCATE);

    /* The validator copies a message into a buffer of the STUN message
     * size. It refuses a longer one. */
    char password[] = "password";
    stun_datagram(BINDING_REQUEST, 0, 1472);
    assert(stun_msg_is_valid(datagram, 1472, password) == -1);
    assert(stun_msg_is_valid(datagram, 20, password) == 0);

    /* A MESSAGE-INTEGRITY attribute of 19 bytes and one byte of padding is
     * not a message without the attribute: both validators refuse it. The
     * stand-in HMAC of this test is 20 zero bytes. */
    enum { BINDING_RESPONSE = 0x0101 };
    stun_datagram(BINDING_RESPONSE, 24, 44);
    put16(datagram + 20, STUN_ATTR_TYPE_MESSAGE_INTEGRITY); put16(datagram + 22, 19);
    assert(stun_msg_is_valid(datagram, 44, password) == -1);
    assert(stun_msg_is_valid_response(datagram, 44, STUN_CREDENTIAL_SHORT_TERM, NULL, NULL, password) == -1);
    /* With all 20 bytes, the validators compare the value with the HMAC. */
    put16(datagram + 22, 20);
    assert(stun_msg_is_valid(datagram, 44, password) == 0);
    assert(stun_msg_is_valid_response(datagram, 44, STUN_CREDENTIAL_SHORT_TERM, NULL, NULL, password) == 0);
    datagram[43] = 1;
    assert(stun_msg_is_valid(datagram, 44, password) == -1);
    assert(stun_msg_is_valid_response(datagram, 44, STUN_CREDENTIAL_SHORT_TERM, NULL, NULL, password) == -1);

    /* An attribute without its value before MESSAGE-INTEGRITY does not hide
     * the attribute from the validator: a Binding request that has the
     * attribute on the wire is not valid without a check of its value. */
    stun_datagram(BINDING_REQUEST, 4 + 24, 48);
    put16(datagram + 20, STUN_ATTR_TYPE_ERROR_CODE);
    put16(datagram + 24, STUN_ATTR_TYPE_MESSAGE_INTEGRITY); put16(datagram + 26, 20);
    datagram[47] = 1;
    assert(stun_msg_is_valid(datagram, 48, password) == -1);
    return 0;
}
'''


class EspPeerStunBoundsTest(unittest.TestCase):
    def test_malformed_datagrams(self) -> None:
        agent = (CORE / 'agent.c').read_text()
        stun = (CORE / 'stun.c').read_text()
        source = '\n'.join([
            PRELUDE,
            *(typedef(agent, name) for name in ('ice_candidate_state_t', 'ice_candidate_pair_t')),
            HARNESS,
            *(function(stun, name) for name in ('stun_get_mapped_address', 'stun_attr_read_size',
                                                'stun_parse_msg_buf', 'stun_probe', 'stun_msg_is_valid',
                                                'stun_msg_is_valid_response')),
            *(function(agent, name) for name in ('is_channel_data', 'agent_recv_one_packet')),
            TESTS,
        ])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-sanitize=alignment', '-fno-sanitize-recover=undefined',
                            # The source prints a size_t with %.*s, which fits the 32-bit target.
                            '-Wno-format',
                            f'-I{CORE}', f'-I{PEER / "include"}', f'-I{PEER / "src"}',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
