import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import url from "node:url";

const root = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), "..");
const source = ["webrtc_transport.c", "webrtc_data_channels.c", "webrtc_transport_rpc_adapters.c", "webrtc_transport_private.h"].map(name => fs.readFileSync(path.join(root, name), "utf8")).join("\n");

const functionBody = name => {
  const start = source.search(new RegExp(`\\b${name}\\s*\\([^)]*\\)\\s*\\{`));
  assert.ok(start >= 0, `${name} must exist`);
  const open = source.indexOf("{", start);
  let depth = 1;
  let cursor = open + 1;
  while (depth > 0 && cursor < source.length) {
    if (source[cursor] === "{") depth++;
    if (source[cursor] === "}") depth--;
    cursor++;
  }
  assert.equal(depth, 0, `unterminated ${name}`);
  return source.slice(open + 1, cursor - 1);
};

const completion = functionBody("complete_accepted_reboot_request");
const responseAccounting = completion.indexOf("reboot_response_sent++");
const eventRender = completion.indexOf("webrtc_reboot_render_will_event");
const eventSend = completion.indexOf("data_channel_send_exact_rpc_peer");
const releaseWorker = completion.indexOf("webrtc_reboot_slot_mark_dispatch_ready");
const notifyWorker = completion.indexOf("xTaskNotifyGive(task)");
assert.ok(responseAccounting >= 0);
assert.ok(responseAccounting < eventRender, "response handling must precede willReboot rendering");
assert.ok(eventRender < eventSend, "willReboot must be rendered before its exact-peer send");
assert.ok(eventSend < releaseWorker, "worker release must follow both send attempts");
assert.ok(releaseWorker < notifyWorker, "slot must be dispatch-ready before task notification");
assert.ok(completion.includes("current_before_event"));
assert.ok(completion.includes("current_after_event"));
assert.ok(completion.includes("both_sends_succeeded"));
assert.ok(
  completion.includes("current_after_event && both_sends_succeeded"),
  "only two successful sends on the still-current peer may release the worker",
);
assert.ok(completion.includes("webrtc_reboot_slot_clear(&ctx->reboot_slot)"));

const peerData = functionBody("peer_on_data");
assert.ok(peerData.includes("jetkvm_control_handle_rpc_with_context"));
assert.ok(peerData.includes("complete_accepted_reboot_request"));
assert.ok(!peerData.includes("esp_restart()"), "peer callback must never restart");
assert.ok(!peerData.includes("vTaskDelay"), "peer callback must never sleep");

const worker = functionBody("reboot_task");
assert.ok(worker.includes("reboot_task_stop_requested"));
assert.ok(
  worker.indexOf("webrtc_reboot_wait_us") < worker.indexOf("esp_restart()"),
  "the restart waits as webrtc_reboot_wait_us() says",
);
assert.ok(
  worker.includes("webrtc_reboot_wait_us(&ctx->reboot_slot"),
  "the wait reads the live slot, which learns when the messages leave the queue",
);
assert.ok(
  worker.includes("cancelled = !ctx->reboot_slot.dispatch_ready"),
  "a reboot whose queued response or willReboot was lost does not restart",
);
assert.ok(
  !worker.includes("reboot_rpc_peer_current_locked"),
  "a closing peer must not cancel a dispatched reboot",
);

const lost = functionBody("queued_message_lost");
assert.ok(
  lost.includes("data_channel_queue_drop_through(&ctx->send_queue, stream_id"),
  "a cancelled reboot drops its willReboot from the send queue",
);

const nextGeneration = functionBody("next_generation_locked");
assert.ok(
  nextGeneration.includes("webrtc_reboot_slot_cancel_generation"),
  "every generation bump must cancel its reboot slot",
);

const exactSend = functionBody("data_channel_send_exact_rpc_peer");
assert.ok(exactSend.includes("reboot_rpc_peer_current_locked"));
const currentGuard = functionBody("reboot_rpc_peer_current_locked");
for (const guard of [
  "active_peer_session == session_id",
  "active_peer_generation == generation",
  "rpc_data_channel_stream_id == stream_id",
  "session_id == session_id",
  "generation == generation",
]) {
  assert.ok(currentGuard.includes(guard), `exact send missing guard: ${guard}`);
}

console.log("reboot response/event/task ordering contract: OK");
