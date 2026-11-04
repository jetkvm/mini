#!/usr/bin/env python3

import pathlib
import re
import subprocess
import tempfile
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
COMPONENT_ROOT = REPO_ROOT / "third_party" / "esp_peer"


class EspPeerSrtpFailClosedTest(unittest.TestCase):

    def test_encrypt_helpers_fail_closed(self) -> None:
        source = (COMPONENT_ROOT / "src" / "dtls_common.h").read_text()
        self.assertIn(
            "srtp_err_status_t status = srtp_protect(", source
        )
        self.assertIn(
            "srtp_err_status_t status = srtp_protect_rtcp(", source
        )
        self.assertEqual(
            source.count("*bytes = esp_peer_srtp_output_bytes((int)status, size);"),
            2,
        )

    def test_packet_egress_paths_are_wrapped(self) -> None:
        cmake = (COMPONENT_ROOT / "CMakeLists.txt").read_text()
        wrapped = set(re.findall(r"-Wl,--wrap=([A-Za-z0-9_]+)", cmake))
        for symbol in (
            "agent_send",
            "agent_send_high_priority",
            "agent_send_to_port",
            "rtp_rolling_buffer_insert",
        ):
            self.assertIn(symbol, wrapped)

    def test_host_semantics_drop_failure_sentinel_before_real_egress(self) -> None:
        harness = r'''
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "srtp_fail_closed.h"

int __wrap_agent_send(void *, const uint8_t *, int);
int __wrap_agent_send_high_priority(void *, const uint8_t *, int);
int __wrap_agent_send_to_port(void *, int, const uint8_t *, int);
int __wrap_rtp_rolling_buffer_insert(void *, uint32_t, uint8_t *, size_t,
                                     uint8_t *);

static unsigned send_calls;
static unsigned high_priority_calls;
static unsigned port_calls;
static unsigned rolling_calls;

int __real_agent_send(void *agent, const uint8_t *packet, int bytes)
{
    (void)agent;
    assert(packet != NULL);
    assert(bytes == 12);
    ++send_calls;
    return 101;
}

int __real_agent_send_high_priority(void *agent, const uint8_t *packet,
                                    int bytes)
{
    (void)agent;
    assert(packet != NULL);
    assert(bytes == 13);
    ++high_priority_calls;
    return 102;
}

int __real_agent_send_to_port(void *agent, int port, const uint8_t *packet,
                              int bytes)
{
    (void)agent;
    assert(port == 3478);
    assert(packet != NULL);
    assert(bytes == 14);
    ++port_calls;
    return 103;
}

int __real_rtp_rolling_buffer_insert(void *rolling, uint32_t uuid,
                                     uint8_t *packet, size_t bytes,
                                     uint8_t *key)
{
    (void)rolling;
    assert(uuid == 0x12345678U);
    assert(packet != NULL);
    assert(bytes == 15);
    assert(key == NULL);
    ++rolling_calls;
    return 104;
}

int main(void)
{
    uint8_t packet[16] = {0};

    assert(esp_peer_srtp_output_bytes(0, 1428) == 1428);
    assert(esp_peer_srtp_output_bytes(1, 1428) == 0);
    assert(esp_peer_srtp_output_bytes(-1, 1428) == 0);
    assert(esp_peer_srtp_output_bytes(0, (size_t)INT_MAX + 1U) == 0);

    assert(__wrap_agent_send(NULL, packet, 0) < 0);
    assert(__wrap_agent_send(NULL, packet, -1) < 0);
    assert(__wrap_agent_send_high_priority(NULL, packet, 0) < 0);
    assert(__wrap_agent_send_to_port(NULL, 3478, packet, 0) < 0);
    assert(__wrap_rtp_rolling_buffer_insert(
               NULL, 0x12345678U, packet, 0, NULL) < 0);
    assert(send_calls == 0);
    assert(high_priority_calls == 0);
    assert(port_calls == 0);
    assert(rolling_calls == 0);

    assert(__wrap_agent_send(NULL, packet, 12) == 101);
    assert(__wrap_agent_send_high_priority(NULL, packet, 13) == 102);
    assert(__wrap_agent_send_to_port(NULL, 3478, packet, 14) == 103);
    assert(__wrap_rtp_rolling_buffer_insert(
               NULL, 0x12345678U, packet, 15, NULL) == 104);
    assert(send_calls == 1);
    assert(high_priority_calls == 1);
    assert(port_calls == 1);
    assert(rolling_calls == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = pathlib.Path(temporary_directory)
            harness_path = temporary_root / "test_srtp_fail_closed.c"
            binary_path = temporary_root / "test_srtp_fail_closed"
            harness_path.write_text(harness)
            subprocess.run(
                [
                    "cc",
                    "-std=c17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{COMPONENT_ROOT / 'src'}",
                    str(COMPONENT_ROOT / "src" / "srtp_fail_closed.c"),
                    str(harness_path),
                    "-o",
                    str(binary_path),
                ],
                check=True,
            )
            subprocess.run([str(binary_path)], check=True)


if __name__ == "__main__":
    unittest.main()
