#!/usr/bin/env python3
"""Check the source-built peer's advertised SCTP capabilities."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / 'third_party/esp_peer/src/core'


def function(text: str, name: str) -> str:
    signature = re.search(r'^[^;\n]*\b' + re.escape(name) + r'\([^;]*?\)\s*\{',
                          text, re.M | re.S)
    if signature is None:
        raise AssertionError(f'missing source function {name}')
    depth = 1
    end = signature.end()
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[signature.start():end]


class EspPeerSourceTransportTest(unittest.TestCase):
    def test_sdp_message_limit_fits_the_receive_reserve(self) -> None:
        limits = re.findall(r'a=max-message-size:(\d+)', (CORE / 'sdp.c').read_text())
        self.assertEqual(limits, ['163840'])
        header = (ROOT / 'components/webrtc_transport/peer_sctp_hid_internal.h').read_text()
        reserve = re.search(r'#define PEER_SCTP_RECEIVE_RESERVE \((\d+)U \* 1024U\)', header)
        self.assertIsNotNone(reserve)
        self.assertLess(int(limits[0]), int(reserve[1]) * 1024)

    def test_init_and_init_ack_advertise_both_supported_extensions(self) -> None:
        source = (CORE / 'sctp.c').read_text()
        writers = '\n'.join(function(source, name) for name in (
            'sctp_client_send_init_chunk', 'sctp_client_send_init_ack_chunk'))
        stream_constants = '\n'.join(re.findall(
            r'^#define\s+SCTP_(?:STREAM_SLOTS|NEGOTIATED_STREAMS)\b.*$', source, re.M))
        harness = stream_constants + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#define LOCAL static
#define SCTP_ALIGN4(n) (((n) + 3) & ~3)
#define LOGI(...) ((void)0)
enum { SCTP_INIT = 1, SCTP_INIT_ACK = 2, SCTP_ASCONF = 130, SCTP_FORWARD_TSN = 192 };
typedef struct __attribute__((packed)) { uint16_t src_port, dst_port; uint32_t verification_tag, checksum; } sctp_header_t;
typedef struct __attribute__((packed)) { uint8_t type, flags; uint16_t length; } sctp_chunk_t;
typedef struct __attribute__((packed)) {
    sctp_chunk_t common;
    uint32_t initiate_tag, wnd_size;
    uint16_t out_stream_num, in_stream_num;
    uint32_t initial_tsn;
    uint8_t param[];
} sctp_init_chunk_t;
typedef struct __attribute__((packed)) { uint16_t type, length; uint8_t value[]; } sctp_chunk_param_t;
typedef struct {
    void *send_lock;
    uint8_t buf[512];
    uint32_t verification_tag, send_tsn;
    struct { uint32_t send_cache_size, recv_cache_size; } cfg;
} sctp_t;
static int locks, writes;
static uint8_t sent[512];
static int sent_size;
static void media_lib_mutex_lock(void *lock, int timeout) { (void)lock; assert(timeout == -1); ++locks; }
static void media_lib_mutex_unlock(void *lock) { (void)lock; --locks; }
static int jetkvm_vendor_fill_header_and_write(sctp_t *sctp, uint8_t *buf, int size)
{
    (void)sctp;
    assert(locks == 1 && size > 0 && (size_t)size <= sizeof(sent));
    memcpy(sent, buf, (size_t)size); sent_size = size; ++writes;
    return 0;
}
''' + writers + r'''
static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void check_extensions(uint8_t type)
{
    const size_t header_size = sizeof(sctp_header_t);
    assert(sent[header_size] == type);
    size_t end = header_size + be16(sent + header_size + 2);
    assert(end <= (size_t)sent_size);
    bool reconfiguration = false, forward_tsn = false;
    int extension_lists = 0;
    for (size_t pos = header_size + sizeof(sctp_init_chunk_t); pos + 4 <= end;) {
        uint16_t kind = be16(sent + pos), size = be16(sent + pos + 2);
        assert(size >= 4 && pos + size <= end);
        if (kind == 0x8008) {
            ++extension_lists;
            assert(size == 6);
            for (size_t i = pos + 4; i < pos + size; ++i) {
                reconfiguration |= sent[i] == 130;
                forward_tsn |= sent[i] == 192;
            }
        }
        pos += (size + 3U) & ~(size_t)3U;
    }
    assert(extension_lists == 1 && reconfiguration && forward_tsn);
}
int main(void)
{
    sctp_t sctp = {.verification_tag = 7, .send_tsn = 13,
                   .cfg = {.send_cache_size = 262144, .recv_cache_size = 262144}};
    assert(sctp_client_send_init_chunk(&sctp) == 0);
    assert(writes == 1 && locks == 0); check_extensions(SCTP_INIT);
    assert(sctp_client_send_init_ack_chunk(&sctp) == 0);
    assert(writes == 2 && locks == 0); check_extensions(SCTP_INIT_ACK);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'test.c').write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(work / 'test.c'),
                            '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
