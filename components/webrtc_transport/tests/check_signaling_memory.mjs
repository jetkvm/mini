import {transportSource as productionTransport} from "./source_helpers.mjs";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import url from "node:url";

const component = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), "..");
const transport = productionTransport;
const http = fs.readFileSync(path.join(component, "webrtc_http.c"), "utf8");
const playout = fs.readFileSync(path.join(component, "playout_delay.c"), "utf8");
const privateHeader = fs.readFileSync(
  path.join(component, "webrtc_transport_private.h"), "utf8");
const budgetHeader = fs.readFileSync(path.join(component, "signaling_budget.h"), "utf8");

function body(source, symbol) {
  const signature = new RegExp(`(?:^|\\n)[^\\n;]*\\b${symbol}\\s*\\(`, "gm");
  let match;
  let open = -1;
  do {
    match = signature.exec(source);
    assert.ok(match, `missing ${symbol}`);
    open = source.indexOf("{", match.index);
    const declaration = source.indexOf(";", match.index);
    if (declaration < 0 || declaration > open) break;
  } while (true);
  let depth = 0;
  for (let i = open; i < source.length; ++i) {
    if (source[i] === "{") depth++;
    if (source[i] === "}" && --depth === 0) return source.slice(open + 1, i);
  }
  assert.fail(`unterminated ${symbol}`);
}

for (const contract of [
  "WEBRTC_SIGNAL_QUEUE_DEPTH 16U",
  "WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES (32U * 1024U)",
  "WEBRTC_SIGNAL_RESIDENT_BUDGET_BYTES \\",
  "JETKVM_MEMORY_CONTROL_SIGNALING_MAX_BYTES",
  "release_bytes > budget->used",
  "budget->used = remaining + claim_bytes",
]) {
  assert.ok(budgetHeader.includes(contract), `missing budget contract: ${contract}`);
}

for (const field of ["void *allocation", "size_t budget_bytes"]) {
  assert.ok(privateHeader.includes(field), `event ownership missing ${field}`);
}

const allocateOutbound = body(transport, "allocate_outbound_event");
for (const contract of [
  "WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES",
  "sizeof(webrtc_signal_event_t) + size + 1U",
  "jetkvm_memory_policy_control_alloc",
  "event->allocation = event",
  "event->data[size] = '\\0'",
]) {
  assert.ok(allocateOutbound.includes(contract), `outbound allocation missing ${contract}`);
}
assert.ok(!/\b(?:malloc|calloc|realloc)\s*\(/.test(allocateOutbound));

const releaseOutbound = body(transport, "free_event_data");
assert.ok(releaseOutbound.includes("jetkvm_memory_policy_control_free(allocation)"));
assert.ok(releaseOutbound.includes("release_signaling_budget(ctx, budget_bytes)"));
assert.ok(!/\bfree\s*\(/.test(releaseOutbound));

const outboundPush = body(transport, "outbound_push");
for (const contract of [
  "webrtc_signaling_budget_try_replace(",
  "replace_bytes",
  "evicted->budget_bytes = 0U",
  "free_event_data(ctx, evicted)",
  "cloud_cb(session_id, generation, cloud_type",
  "copy->data",
]) {
  assert.ok(outboundPush.includes(contract), `outbound queue missing ${contract}`);
}

const inboundSubmit = body(transport, "webrtc_transport_session_submit");
for (const contract of [
  "sizeof(inbound_signal_t) + size + 1U",
  "jetkvm_memory_policy_control_alloc",
  "webrtc_signaling_budget_claim(",
  "signal->budget_bytes = resident_bytes",
  "free_inbound_signal(ctx, signal)",
  "ctx->stats.signaling_messages_dropped++",
]) {
  assert.ok(inboundSubmit.includes(contract), `inbound ownership missing ${contract}`);
}
assert.ok(!/\b(?:malloc|calloc|realloc|free)\s*\(/.test(inboundSubmit));

const releaseInbound = body(transport, "free_inbound_signal");
assert.ok(releaseInbound.includes("jetkvm_memory_policy_control_free(signal)"));
assert.ok(releaseInbound.includes("release_signaling_budget(ctx, budget_bytes)"));

const poll = body(transport, "session_poll_with_role");
assert.ok(poll.includes("*event = *next"));
assert.ok(!poll.includes("budget_bytes = 0"),
  "HTTP handoff must stay charged until explicit event release");
const publicRelease = body(transport, "webrtc_transport_signal_event_release");
assert.ok(publicRelease.includes("free_event_data(webrtc_transport_context(), event)"));

const peerCallback = body(transport, "peer_on_message");
assert.ok(peerCallback.includes("WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES"));
assert.ok(peerCallback.includes("record_signaling_drop(ctx)"));

const receiveBody = body(http, "receive_body");
assert.ok(receiveBody.includes("jetkvm_memory_policy_control_alloc"));
assert.ok(receiveBody.includes("jetkvm_memory_policy_control_free(buffer)"));
assert.ok(!/\b(?:malloc|calloc|realloc|free)\s*\(/.test(receiveBody));
const signalHandler = body(http, "signal_handler");
assert.ok(signalHandler.includes("jetkvm_memory_policy_control_free(body)"));
assert.ok(signalHandler.includes("webrtc_transport_signal_record_drop()"));

const freeIce = body(transport, "owned_ice_server_list_free");
assert.ok(freeIce.includes("jetkvm_memory_policy_control_free"));
assert.ok(!/\bfree\s*\(/.test(freeIce));
const copyIce = body(transport, "copy_bounded_string");
assert.ok(copyIce.includes("jetkvm_memory_policy_control_alloc"));
assert.ok(!/\b(?:malloc|calloc|realloc)\s*\(/.test(copyIce));
const createIce = body(transport, "owned_ice_server_list_create");
assert.ok(createIce.includes("jetkvm_memory_policy_control_alloc(sizeof(*list))"));
assert.ok(createIce.includes("memset(list, 0, sizeof(*list))"));
assert.ok(!/\b(?:malloc|calloc|realloc)\s*\(/.test(createIce));

for (const contract of [
  "WEBRTC_CONTROL_ICE_STRING_BOUND_BYTES",
  "WEBRTC_CONTROL_SDP_COPY_BOUND_BYTES",
  "JETKVM_MEMORY_CONTROL_RUNTIME_HEADROOM_BYTES / 4U",
  ".alloc = jetkvm_memory_policy_control_alloc",
  ".free = jetkvm_memory_policy_control_free",
  "&s_playout_delay_allocator, &answer",
  "&s_playout_delay_allocator, &offer",
]) {
  assert.ok(transport.includes(contract), `ICE/SDP isolation missing ${contract}`);
}
assert.ok(playout.includes("allocator->alloc"));
assert.ok(playout.includes("offer->free_fn(offer->data)"));
assert.ok(!/\b(?:malloc|calloc|realloc)\s*\(/.test(playout));

console.log("bounded control-arena signaling ownership contract: OK");
