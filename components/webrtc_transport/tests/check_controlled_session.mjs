import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const component = resolve(here, '..');
const source = ['webrtc_transport.c', 'webrtc_sessions.c', 'webrtc_data_channels.c']
  .map(file => readFileSync(resolve(component, file), 'utf8')).join('\n');
const header = readFileSync(
  resolve(component, 'include', 'webrtc_transport.h'), 'utf8');
const peerHeader = readFileSync(resolve(
  component, '..', '..', 'third_party', 'esp_peer',
  'include', 'esp_peer_types.h'), 'utf8');

function extractCFunction(text, name) {
  const signature = new RegExp(`(?:^|\\n)[^\\n;]*\\b${name}\\s*\\(`, 'gm');
  let match;
  let open = -1;
  do {
    match = signature.exec(text);
    assert(match, `${name} not found`);
    open = text.indexOf('{', match.index);
    const declaration = text.indexOf(';', match.index);
    if (declaration < 0 || declaration > open) break;
  } while (true);
  assert(open >= 0, `${name} body not found`);
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    if (text[i] === '{') depth++;
    if (text[i] === '}' && --depth === 0) return text.slice(match.index, i + 1);
  }
  assert.fail(`${name} body is unterminated`);
}

for (const token of [
  'WEBRTC_TRANSPORT_CLOUD_SESSION_API_VERSION',
  'WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING',
  'WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED',
  'WEBRTC_TRANSPORT_CLOUD_SIGNAL_ANSWER',
  'WEBRTC_TRANSPORT_CLOUD_SIGNAL_CANDIDATE',
  'WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_PEER_READY',
  'WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_GENERATION_INVALIDATED',
  'WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_SESSION_CLOSED',
  'webrtc_transport_ice_server_t',
  'ice_server_count',
  'deeply copies the list',
  'webrtc_transport_cloud_session_create',
  'webrtc_transport_cloud_session_submit_offer',
  'webrtc_transport_cloud_session_submit_candidate',
  'webrtc_transport_cloud_session_close',
  'valid only until the callback',
]) {
  assert(header.includes(token), `public controlled-session contract missing ${token}`);
}

assert(peerHeader.includes('ESP_PEER_ROLE_CONTROLLED'),
  'esp_peer does not expose the controlled ICE role');

const openPeer = extractCFunction(source, 'open_peer');
for (const token of [
  'role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED',
  'ESP_PEER_ROLE_CONTROLLED',
  'ESP_PEER_ROLE_CONTROLLING',
  '.manual_ch_create = true',
  'ctx->session_role == role',
  'ctx->active_peer_role = role',
  'peer_cfg.server_lists = active_ice_servers',
  'peer_cfg.server_num = active_ice_servers',
  'esp_peer_new_connection(peer)',
  'dispatch_cloud_peer_ready(ctx, session_id, generation)',
]) {
  assert(openPeer.includes(token), `open_peer role/generation guard missing ${token}`);
}

const localCreate = extractCFunction(source, 'webrtc_transport_session_create');
assert(localCreate.includes('WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING'),
  'legacy HTTP session must always select LOCAL_CONTROLLING');
assert(!localCreate.includes('WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED'),
  'legacy HTTP session must not select CLOUD_CONTROLLED');

const cloudCreate = extractCFunction(
  source, 'webrtc_transport_cloud_session_create');
assert(cloudCreate.includes('WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED'),
  'cloud session must explicitly opt into CLOUD_CONTROLLED');
assert(cloudCreate.includes('cfg->on_signal'),
  'cloud session must require an outbound signaling callback');
assert(cloudCreate.includes('owned_ice_server_list_create('),
  'cloud create must deeply copy its session-owned ICE server list');
assert(cloudCreate.includes('cfg->on_lifecycle'),
  'cloud create must install its lifecycle callback');

const processInbound = extractCFunction(source, 'process_inbound');
const applyOffer = processInbound.indexOf('apply_controlled_remote_offer(');
const sendMessage = processInbound.indexOf('esp_peer_send_msg(peer, &message)');
assert(applyOffer >= 0 && sendMessage > applyOffer,
  'controlled offer policy must be committed before esp_peer can synchronously answer');
assert(processInbound.includes(
  'role == WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING'),
  'legacy answer-acceptance path must remain role isolated');
for (const token of [
  'xQueuePeek(ctx->inbound_queue',
  'webrtc_inbound_classify(',
  'WEBRTC_INBOUND_DEFER_CURRENT',
  'WEBRTC_INBOUND_DROP_STALE',
  'WEBRTC_INBOUND_DELIVER_CURRENT',
]) {
  assert(processInbound.includes(token),
    `current-generation inbound ownership missing ${token}`);
}

const peerTask = extractCFunction(source, 'peer_task');
const rebuildStart = peerTask.indexOf('if (rebuild)');
const rebuildEnd = peerTask.indexOf('dispatch_pending_cloud_invalidation(ctx)');
assert(rebuildStart >= 0 && rebuildEnd > rebuildStart,
  'peer rebuild section not found');
assert(!peerTask.slice(rebuildStart, rebuildEnd).includes('flush_inbound(ctx)'),
  'peer rebuild must not flush a just-submitted current offer/candidate');

const applyControlledOffer = extractCFunction(
  source, 'apply_controlled_remote_offer');
for (const token of [
  'webrtc_playout_delay_offer_accepts_sender',
  'signal->generation == ctx->active_peer_generation',
  'ctx->active_peer_role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING',
  'ctx->active_playout_delay_negotiated = false',
]) {
  assert(applyControlledOffer.includes(token),
    `controlled inbound offer guard missing ${token}`);
}

const publishAnswer = extractCFunction(
  source, 'peer_publish_controlled_answer');
for (const token of [
  'webrtc_playout_delay_prepare_answer',
  'WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING',
  'ctx->active_peer_generation == answer_generation',
  'ctx->active_playout_delay_extension_id == extension_id',
  'outbound_push(ctx, WEBRTC_SIGNAL_SDP',
]) {
  assert(publishAnswer.includes(token),
    `controlled outbound answer guard missing ${token}`);
}

const outbound = extractCFunction(source, 'outbound_push');
for (const token of [
  'ctx->active_peer_generation != ctx->generation',
  'ctx->active_peer_role != ctx->session_role',
  'cloud_cb(session_id, generation, cloud_type',
  'webrtc_cloud_callback_gate_try_begin(',
  'webrtc_cloud_callback_gate_end(',
  'WEBRTC_TRANSPORT_CLOUD_SIGNAL_ANSWER',
  'WEBRTC_TRANSPORT_CLOUD_SIGNAL_CANDIDATE',
]) {
  assert(outbound.includes(token), `outbound generation/callback guard missing ${token}`);
}

const expire = extractCFunction(source, 'expire_stale_session');
assert(expire.includes('ctx->session_role == WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING'),
  'HTTP poll timeout must only expire diagnostic HTTP sessions, never WebSocket or cloud sessions');

const poll = extractCFunction(source, 'webrtc_transport_session_poll');
assert(poll.includes('WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING'),
  'HTTP poll must not consume controlled-session callback signaling');

const cloudClose = extractCFunction(
  source, 'webrtc_transport_cloud_session_close');
for (const token of [
  'ctx->cloud_close_pending = true',
  'wait_cloud_callback_idle()',
  'WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_SESSION_CLOSED',
  'webrtc_cloud_callback_gate_try_begin(',
  'ctx->cloud_close_pending = false',
]) {
  assert(cloudClose.includes(token),
    `cloud close callback serialization missing ${token}`);
}

const invalidation = extractCFunction(
  source, 'dispatch_pending_cloud_invalidation');
for (const token of [
  'WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_GENERATION_INVALIDATED',
  'replacement_generation = ctx->generation',
  'webrtc_cloud_callback_gate_try_begin(',
]) {
  assert(invalidation.includes(token),
    `generation lifecycle dispatch missing ${token}`);
}

for (const token of [
  '.on_channel_open = peer_on_channel_open',
  '.on_data = peer_on_data',
  '.on_channel_close = peer_on_channel_close',
]) {
  assert(openPeer.includes(token),
    `browser-created DataChannel callback missing ${token}`);
}

console.log('controlled/browser-offerer transport contracts: OK');
