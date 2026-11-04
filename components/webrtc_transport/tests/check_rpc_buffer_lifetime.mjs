import {transportSource as productionTransport} from "./source_helpers.mjs";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import url from "node:url";

const component = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), "..");
const source = productionTransport;
const control = ["jetkvm_control.c", "rpc_virtual_media.c"]
  .map((name) =>
    fs.readFileSync(
      path.resolve(component, `../control_protocol/${name}`),
      "utf8",
    ),
  )
  .join("\n");

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

assert.ok(source.includes("JETKVM_MEMORY_RPC_SCRATCH_BYTES"));
assert.ok(!source.includes("#define JETKVM_RPC_RESPONSE_MAX"));
assert.ok(source.includes("char *rpc_scratch;"));
assert.ok(source.includes("sizeof(jetkvm_virtual_media_file_list_t) <="));

const received = body("peer_on_data");
assert.ok(received.includes("char *response = ctx->rpc_scratch;"));
assert.ok(!received.includes("heap_caps_malloc"));
assert.ok(!received.includes("heap_caps_aligned_alloc"));
assert.ok(!received.includes("free(response)"));
assert.ok(!received.includes("heap_caps_free(response)"));

const listed = body("rpc_list_virtual_media_files_adapter");
assert.ok(listed.includes("transport->rpc_scratch"));
assert.ok(listed.includes("jetkvm_virtual_media_list_files(files)"));
assert.ok(listed.includes("visit(&file, visit_ctx)"));
assert.ok(!listed.includes("calloc"));
assert.ok(!listed.includes("malloc"));
assert.ok(!listed.includes("free("));
assert.ok(!control.includes("calloc(1U, sizeof(*files))"));

const started = body("webrtc_transport_start");
assert.ok(started.includes("ctx->rpc_scratch = heap_caps_aligned_alloc("));
assert.ok(started.includes("heap_caps_free(ctx->rpc_scratch);"));
assert.ok(started.includes("ctx->rpc_scratch = NULL;"));

const stopped = body("webrtc_transport_stop_internal");
const peerExit = stopped.indexOf("xSemaphoreTake(ctx->task_done");
const scratchFree = stopped.indexOf("heap_caps_free(ctx->rpc_scratch)");
assert.ok(peerExit >= 0 && scratchFree > peerExit,
  "RPC scratch must remain alive until the peer callback task exits");
assert.ok(stopped.includes("ctx->rpc_scratch = NULL;"));

console.log("fixed RPC scratch and zero-copy file-list contract: OK");
