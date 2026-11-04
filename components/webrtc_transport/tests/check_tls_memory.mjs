import {transportSource as productionTransport} from "./source_helpers.mjs";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const source = productionTransport;

for (const contract of [
  "peer_tls_reservation",
  "peer_tls_release_pending",
  '"webrtc-dtls"',
  "jetkvm_memory_policy_reserve_tls_session",
  "release_completed_peer_tls_reservation(ctx)",
  "jetkvm_memory_policy_release(&peer_tls_reservation)",
  "jetkvm_memory_policy_release(&published_tls_reservation)",
]) {
  assert.ok(source.includes(contract), `missing peer TLS contract: ${contract}`);
}

assert.ok(
  source.includes("state == ESP_PEER_STATE_CLOSED ||"),
  "remote CLOSED must enter the rebuild path and release the DTLS ticket",
);

const openPeer = source.slice(
  source.indexOf("static esp_err_t open_peer("),
  source.indexOf("static void apply_controlled_remote_offer("),
);
const peerMemory = openPeer.slice(
  openPeer.indexOf("const jetkvm_memory_policy_request_t peer_memory"),
  openPeer.indexOf('"webrtc-peer"'),
);
assert.ok(
  !peerMemory.includes("internal_bytes") && !peerMemory.includes("dma_bytes"),
  "peer PSRAM ticket must not duplicate the session's internal reservation",
);

assert.match(
  openPeer,
  /bool tls_reserved = jetkvm_memory_policy_reserve_tls_session\(\s*"webrtc-dtls", with_ice_servers,/,
  "the peer's open takes the session's reservation",
);
assert.match(
  openPeer,
  /if \(!tls_reserved\)\s*\{\s*jetkvm_memory_policy_release\(&peer_memory_reservation\);/,
  "failed DTLS admission must release the pending peer PSRAM ticket",
);

console.log("peer DTLS connect-window memory ownership contract: OK");
