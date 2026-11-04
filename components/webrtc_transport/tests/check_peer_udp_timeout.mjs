import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const source = fs.readFileSync(
  path.join(here, "..", "peer_udp_timeout_override.c"),
  "utf8",
);
const root = path.resolve(here, "..", "..", "..");
const peer = path.join(root, "third_party", "esp_peer");
const udpSource = fs.readFileSync(path.join(peer, "src/transport/udp.c"), "utf8");
const udpHeader = fs.readFileSync(path.join(peer, "src/transport/udp.h"), "utf8");
const policyHeader = fs.readFileSync(path.join(peer, "include/esp_peer_udp_policy.h"), "utf8");

const start = source.indexOf("void udp_blocking_timeout(");
const end = source.indexOf("void webrtc_peer_udp_timeout_override_init(");
assert.ok(start >= 0 && end > start, "missing UDP timeout override");
const override = source.slice(start, end);

assert.ok(
  override.includes("ms == CONFIG_WEBRTC_TRANSPORT_AGENT_RECV_TIMEOUT_MS"),
  "only the configured agent wait may be clamped",
);
assert.ok(
  override.includes("ms < portTICK_PERIOD_MS") &&
    override.includes("effective_ms = portTICK_PERIOD_MS"),
  "a sub-tick agent wait must become one scheduler tick",
);
assert.ok(
  override.includes("timeout_sec = effective_ms / 1000") &&
    override.includes("timeout_usec = (long)(effective_ms % 1000) * 1000"),
  "the socket timeout must use the handshake-safe effective wait",
);
assert.ok(
  !/timeout_(?:sec|usec)\s*=\s*0\s*;/.test(override),
  "the DTLS receive path must not be converted to an empty poll",
);

for (const contract of [
  "webrtc_peer_udp_rx_slice_begin",
  "udp_socket_recvfrom_poll_allowed",
  "s_peer_rx_slice.remaining == 0U",
  "*nowait = true",
  "s_peer_rx_slice.quota_exhaustions++",
  "webrtc_peer_udp_rx_slice_end",
]) {
  assert.ok(source.includes(contract), `missing bounded peer receive policy: ${contract}`);
}
const receiveStart = udpSource.indexOf("int WEAK udp_socket_recvfrom_nowait(");
assert.ok(receiveStart >= 0, "missing source-built UDP receive entry");
const receive = udpSource.slice(receiveStart, udpSource.indexOf("\n}", receiveStart));
assert.ok(
  receive.includes("if (!udp_socket_recvfrom_poll_allowed(&nowait))") &&
    receive.indexOf("udp_socket_recvfrom_poll_allowed") < receive.indexOf("udp_add_user") &&
    receive.indexOf("udp_socket_recv_dispatch") < receive.indexOf("udp_socket_recvfrom_poll_complete(ret)"),
  "the UDP receive path must apply the receive policy before dispatch and report its result",
);
assert.ok(
  udpHeader.includes('#include "esp_peer_udp_policy.h"') &&
    source.includes('#include "esp_peer_udp_policy.h"') &&
    !source.includes('#include "transport/udp.h"'),
  "the UDP transport and application must share the public ESP Peer policy boundary",
);
for (const contract of [
  "} udp_socket_t;",
  "void udp_blocking_timeout(udp_socket_t *udp_socket, long long int ms);",
  "bool udp_socket_recvfrom_poll_allowed(bool *nowait);",
  "void udp_socket_recvfrom_poll_complete(int result);",
]) {
  assert.ok(policyHeader.includes(contract), `missing public UDP policy contract: ${contract}`);
}

console.log("peer UDP handshake timeout and candidate receive policy: OK");
