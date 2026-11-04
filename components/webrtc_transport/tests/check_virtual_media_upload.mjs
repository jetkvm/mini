import {transportSource as source} from "./source_helpers.mjs";
import assert from "node:assert/strict";

function body(symbol) {
  const start = source.search(new RegExp(`\\b${symbol}\\s*\\([^)]*\\)\\s*\\{`));
  assert.ok(start >= 0, `missing ${symbol}`);
  const open = source.indexOf("{", start);
  let depth = 1;
  let cursor = open + 1;
  while (depth > 0 && cursor < source.length) {
    if (source[cursor] === "{") depth++;
    if (source[cursor] === "}") depth--;
    cursor++;
  }
  assert.equal(depth, 0, `unterminated ${symbol}`);
  return source.slice(open + 1, cursor - 1);
}

const opened = body("peer_on_channel_open");
for (const contract of [
  "reject_channel = true",
  "close_data_channel(ctx, channel->label)",
  'strcmp(channel->label, "rpc")',
  'strcmp(channel->label, "hidrpc")',
]) {
  assert.ok(opened.includes(contract), `missing fail-closed channel contract: ${contract}`);
}
assert.ok(
  opened.indexOf("reject_channel = true") < opened.indexOf("close_data_channel(ctx, channel->label)"),
  "unsupported channels must be rejected before their close is dispatched",
);

const startedUpload = body("rpc_start_virtual_media_upload_adapter");
for (const contract of [
  "jetkvm_virtual_media_start_upload",
  "upload.already_uploaded_bytes",
  "upload.upload_id",
  "/storage/upload",
]) {
  assert.ok(startedUpload.includes(contract), `missing HTTP upload-session contract: ${contract}`);
}
for (const forbiddenBinding of ["transport->", "portENTER_CRITICAL", "esp_timer_", "abort_upload"]) {
  assert.ok(
    !startedUpload.includes(forbiddenBinding),
    `startStorageFileUpload binds to WebRTC lifecycle: ${forbiddenBinding}`,
  );
}

const peerTask = body("peer_task");
for (const contract of [
  "webrtc_peer_udp_rx_slice_begin",
  "webrtc_peer_udp_rx_slice_end",
  "signaling_needs_receive_fairness",
  "esp_peer_main_loop(ctx->peer)",
]) {
  assert.ok(peerTask.includes(contract), `missing candidate receive-fairness contract: ${contract}`);
}
assert.ok(
  peerTask.includes("signaling_needs_receive_fairness || media_connected") &&
    peerTask.includes("media_connected ? 8U : 1U"),
  "peer loop must retain bounded media polling and one-attempt ICE fairness",
);

console.log("virtual-media HTTP-only transport contract: OK");
