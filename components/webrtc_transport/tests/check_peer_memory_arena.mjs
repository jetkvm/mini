import {transportSource as productionTransport} from "./source_helpers.mjs";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import url from "node:url";

const component = path.resolve(
  path.dirname(url.fileURLToPath(import.meta.url)), "..");
const arena = fs.readFileSync(
  path.join(component, "peer_memory_arena.c"), "utf8");
const transport = productionTransport;

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
  for (let index = open; index < source.length; ++index) {
    if (source[index] === "{") depth++;
    if (source[index] === "}" && --depth === 0) {
      return source.slice(open + 1, index);
    }
  }
  assert.fail(`unterminated ${symbol}`);
}

assert.ok(arena.includes("static atomic_bool s_teardown_failure_reported"));
const claim = body(arena, "claim_teardown_failure_report");
assert.ok(claim.includes("atomic_compare_exchange_strong_explicit"));
assert.ok(claim.includes("atomic_fetch_add_explicit(&s_leak_events"));
assert.equal(
  (arena.match(/atomic_fetch_add_explicit\(&s_leak_events/g) || []).length,
  2,
  "teardown has one latched counter and media_lib_free rejects an arena pointer after teardown",
);

const begin = body(arena, "webrtc_peer_memory_arena_begin");
const cleanup = begin.indexOf("webrtc_peer_memory_arena_end()");
const memoryBackoff = begin.indexOf("return ESP_ERR_NO_MEM", cleanup);
assert.ok(cleanup >= 0 && memoryBackoff > cleanup,
  "a retained prior arena must enter the transport's memory backoff");
assert.ok(begin.includes("atomic_store_explicit(&s_teardown_failure_reported, false"),
  "only a new arena generation may re-arm teardown reporting");

const end = body(arena, "webrtc_peer_memory_arena_end");
assert.equal(
  (end.match(/claim_teardown_failure_report\(\)/g) || []).length,
  2,
  "active operations and retained blocks must share one teardown report latch",
);
assert.ok(end.includes("preserving backing memory"));
assert.ok(!end.includes("heap_caps_free(base)") ||
  end.indexOf("heap_caps_free(base)") > end.indexOf("info.allocated_blocks"));

const closeHelper = body(transport, "close_peer_handle_and_flush_outbound");
const closeIndex = closeHelper.indexOf("esp_peer_close(peer)");
const flushIndex = closeHelper.indexOf("flush_outbound(ctx)");
assert.ok(0 <= closeIndex && closeIndex < flushIndex,
  "peer close must own disconnect before signaling is discarded");
assert.ok(!closeHelper.includes("esp_peer_disconnect(peer)"),
  "peer_close already disconnects the vendor graph; do not tear it down twice");
assert.ok(!closeHelper.includes("peer arena teardown failed"),
  "the arena owner must emit the sole precise teardown failure log");

assert.ok(!transport.includes("webrtc_peer_memory_arena_begin("),
  "the mixed vendor allocator graph cannot be placed in a partial private heap");
assert.ok(!transport.includes("webrtc_peer_memory_arena_end("),
  "transport teardown must leave vendor allocations in their original heap domain");

const retryDelay = transport.indexOf("open_ret == ESP_ERR_NO_MEM ?");
assert.ok(retryDelay >= 0);
assert.ok(transport.indexOf("JETKVM_MEMORY_RETRY_DELAY_MS", retryDelay) >
  retryDelay);

console.log("peer arena owner teardown and retained-memory backoff contract: OK");
