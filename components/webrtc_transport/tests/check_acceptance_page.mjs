import {transportSource as productionTransport} from "./source_helpers.mjs";
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';
import {runInNewContext} from 'node:vm';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(resolve(here, '..', 'acceptance_page.html'), 'utf8');
const transportSource = productionTransport;
const httpSource = ['webrtc_http.c', 'webrtc_stats_json.c'].map(name => readFileSync(resolve(here, '..', name), 'utf8')).join('\n');
const networkTimingSource = readFileSync(
  resolve(here, '..', 'network_timing.c'), 'utf8');
const networkTimingWrappers = readFileSync(
  resolve(here, '..', 'network_timing_wrappers.c'), 'utf8');
const playoutDelaySource = readFileSync(
  resolve(here, '..', 'playout_delay.c'), 'utf8');
const playoutDelayHeader = readFileSync(
  resolve(here, '..', 'playout_delay.h'), 'utf8');
const publicHeader = readFileSync(
  resolve(here, '..', 'include', 'webrtc_transport.h'), 'utf8');
const frameMetaHeader = publicHeader;
const captureSource = readFileSync(resolve(here, '..', '..', 'jetkvm_video',
  'jetkvm_video.c'), 'utf8');
const controlProtocolSource = ['jetkvm_control.c', 'jetkvm_hidrpc.c'].map(
  (name) => readFileSync(resolve(here, '..', '..', 'control_protocol', name),
    'utf8')).join('\n');
const h264ParamSource = readFileSync(resolve(here, '..', '..', '..',
  'managed_components', 'espressif__esp_h264', 'hw', 'src',
  'esp_h264_enc_hw_param.c'), 'utf8');
const projectCmake = readFileSync(resolve(here, '..', '..', '..',
  'CMakeLists.txt'), 'utf8');
const match = html.match(/<script>([\s\S]*?)<\/script>/i);
if (!match) throw new Error('inline acceptance script not found');
new Function(match[1]);

function extractCFunction(source, name) {
  let start = -1;
  let open = -1;
  let searchFrom = 0;
  const functionPattern = new RegExp(`\\b${name}\\s*\\(`, 'g');
  for (;;) {
    functionPattern.lastIndex = searchFrom;
    const match = functionPattern.exec(source);
    if (!match) throw new Error(`C function ${name} not found`);
    start = match.index;
    open = source.indexOf('{', start);
    if (open < 0) throw new Error(`C function ${name} body not found`);
    const semicolon = source.indexOf(';', start);
    if (semicolon < 0 || open < semicolon) break;
    searchFrom = functionPattern.lastIndex;
  }
  let depth = 0;
  for (let index=open; index<source.length; index++) {
    if (source[index] === '{') depth++;
    if (source[index] === '}' && --depth === 0) return source.slice(start, index + 1);
  }
  throw new Error(`C function ${name} is unterminated`);
}

for (const required of [
  'window.kvmAcceptance', 'requestVideoFrameCallback', 'getStats()',
  "peer.createDataChannel('rpc', {ordered:true})",
  "['ping', {}, value => value === 'pong']",
  "['getVideoState'", "['getLocalVersion'", "['getNetworkSettings'",
  "['getAudioConfig'", "['getStreamQualityFactor'",
  "['getVideoCodecPreference'",
  'USB/HID channels intentionally not created',
  'jetkvmControl:{', "version:'1.4.0'",
  "new URLSearchParams(location.search).get('display')",
  "classList.toggle('bare-receiver', DISPLAY_PROFILE.bare)",
  "'bare-fill-receiver', DISPLAY_PROFILE.fillViewport",
  'displayProfile:{...DISPLAY_PROFILE}',
  'DISPLAY_PROFILE.continuousLocalClockRafEnabled',
  'DISPLAY_PROFILE.domTelemetryUpdatesEnabled',
  'suppressedByDisplayProfile:DISPLAY_PROFILE.bare',
  'framesDecoded', 'framesDropped', 'freezeCount', 'fingerprintCanvas',
  'toDataURL', '1920 && height === 1080', '1280 && height === 720',
  "name:'720p30'", "name:'720p60'", 'threshold:28', 'threshold:55',
  "telemetryFps = requestedMode === '720p60' ? 60 : 30",
  'Number(state.firmware?.fps) || telemetryFps',
  '/webrtc/session', '/webrtc/events',
  '/webrtc/signal', '/webrtc/stats',
  'counterSources', 'selectedCandidatePair:liveNetwork',
  'captured:Math.trunc', 'encoded:Math.trunc', 'rtpSent:Math.trunc',
  'decoded:Math.trunc', 'unique:classified - duplicate',
  'unclassified:rendered - classified',
  'filterLocalCandidate', 'accepted-same-ipv4-/24-host',
  'stripUnfilteredCandidates', "line.startsWith('a=candidate:')",
  'localCandidates:[...state.localCandidates]',
  'fingerprintCallbackWorkAverageMs', 'fingerprintCallbackWorkMaximumMs',
  'fingerprintCallbackObservedFrameSkips', 'compositorPresentedFrames',
  'new VideoFrame(video)', 'frame.copyTo', 'decodeTelemetryRow',
  'telemetryGeometry', 'pixelLuma', 'median', 'median3',
  'grayToBinary',
  'identityDecodeAnomalies', 'fingerprintUnclassifiedFrames',
  'pageLifecycleId', 'startInvocationCount', 'peerInstanceSequence',
  'trackEventSequence', 'trackEvents:state.trackEvents.map',
  'activeTrack:describeActiveTrack()', 'liveInboundIdentity',
  'describeReceiverLatencyControl', "'browser-default'",
  'describeReceiverLatencyExperiment',
  "new URLSearchParams(location.search).get('lowLatency')",
  "new URLSearchParams(location.search).get('senderPlayoutDelay')",
  "queryValue === '1'", "queryValue === 'jitter'", "queryValue === 'playout'",
  'RECEIVER_LATENCY_EXPERIMENT.jitterBufferTargetRequested',
  'RECEIVER_LATENCY_EXPERIMENT.playoutDelayHintRequested',
  'IDENTITY_CAPTURE_POLICY.requested',
  "new URLSearchParams(location.search).get('identityCapture')",
  "'receiver-zero-delay-hints'", 'receiverLatencyExperiment:{...RECEIVER_LATENCY_EXPERIMENT}',
  'receiverLatencyControl:state.activeTrack ?', 'requested,',
  'senderPlayoutDelayPolicy:{...SENDER_PLAYOUT_DELAY_POLICY}',
  'senderPlayoutDelaySessions:state.senderPlayoutDelaySessions.map',
  "'/webrtc/session?senderPlayoutDelay=off'",
  'mutated:false', 'jitterBufferTargetRequested:jitterRequested',
  'jitterBufferTargetRequestedValue:jitterRequested ? 0 : null',
  'jitterBufferTargetReadback:null', 'jitterBufferTargetUntouched:true',
  'playoutDelayHintRequested:playoutRequested',
  'playoutDelayHintRequestedValue:playoutRequested ? 0 : null',
  'playoutDelayHintReadback:null',
  'playoutDelayHintUntouched:true', 'describeJitterBuffer',
  'averageTargetDelayMs', 'averageMinimumDelayMs',
  'serializeRtcStats', 'stats:includeFullStats ? serializeRtcStats(liveReport)',
  'describeInboundIdentity', 'identityTrackToken', 'beginIdentityTrack',
  'decodedCounterTotal', 'cumulative high-water sum',
  'ssrc:finiteNumberOrNull(inbound.ssrc)',
  'trackIdentifier:inbound.trackIdentifier', 'codecId:inbound.codecId',
  'TELEMETRY_ROW_FRACTION', 'TELEMETRY_CELL_COUNT = 96',
  'metadata.expectedDisplayTime',
  'metadata.receiveTime', 'metadata.processingDuration',
  'observePresentationLedger(callbackNow, metadata)',
  "version:'webrtc-rvfc-presentation-ledger/v1'",
  'receiveToPresentationMs', 'nonDecodeResidualToPresentationMs',
  'presentationToExpectedDisplayMs', 'receiveToExpectedDisplayMs',
  'callbackAfterPresentationMs', 'callbackToExpectedDisplayMs',
  'buildPresentationStatsInterval', 'totalProcessingDelay',
  'framesAssembledFromMultiplePackets',
  'presentationLedger:presentationLedgerSummary(includeSamples)',
  'opticalDisplayMeasured:false',
  'performance.timeOrigin + expectedDisplayTime', 'unsignedDelta32',
  'MAX_VALID_LATENCY_MS', 'histogramPercentile', 'latencySummary',
  'validSampleCount', 'invalidSampleCount', 'sourceClockWraps',
  'histogramMs', 'histogramSampleCount', 'histogramWeightedSumMs',
  'histogramCountMatches', 'histogramSumMatches', 'sparseHistogramSummary',
  'frameAgeProxy:latencySummary()',
  "version:'kvm-same-host-gray32-frame-age/v3'",
  "telemetryLayoutVersion:'packed-gray96-crc32c/v1'",
  'measurementOnly:true',
  "receiverTimestamp:'floor(performance.timeOrigin + rVFC metadata.expectedDisplayTime)'",
  "measurementName:'source-to-receiver-compositor frame-age proxy'",
  'sameHostRequired:true', 'glassToGlass:false',
  "glassToGlassRequirement:'requires an external camera observing both physical displays'",
  'decoderSanityWindowIsNotAcceptanceThreshold:true',
  'TIMECODE_MAGIC', 'crc32cWords', 'telemetryCrc32c', 'wireCrc32c',
  'TELEMETRY_MIN_CONTRAST', 'TELEMETRY_MIN_CELL_DISTANCE',
  "rejectLatency('telemetryLowConfidence')",
  "rejectLatency('timecodeIntegrity')", 'height:1',
  'browserClockSample', 'browserClock = browserClockSample()',
  'performanceTimeOrigin:performance.timeOrigin', 'performanceNow',
  'epochMs', 'dateNowMs', 'clockSample:browserClockSample',
  'await captureScreenshot()', 'screenshotContext.drawImage(frame',
]) {
  if (!html.includes(required)) throw new Error(`missing acceptance contract token: ${required}`);
}
for (const required of [
  '.enable_data_channel = true', '.manual_ch_create = true',
  '.on_channel_open = peer_on_channel_open', '.on_data = peer_on_data',
  '.on_channel_close = peer_on_channel_close',
  'CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_SEND_CACHE_BYTES',
  'CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_RECEIVE_CACHE_BYTES',
  'rpc_terminal_video_event_pending', 'send_terminal_video_event(ctx)',
  'send_one_pending_hidrpc_state(ctx, now_us)',
]) {
  if (!transportSource.includes(required)) {
    throw new Error(`missing JetKVM DataChannel transport token: ${required}`);
  }
}
for (const required of [
  'jetkvmDataChannels', 'rpcOpen', 'hidrpcOpen', 'rpcResponses',
  'hidrpcHandshakes', 'generation-gated HIDRPC v1', 'P4 HS/UTMI',
  'hidrpcUnreliableOrderedOpen', 'hidrpcUnreliableUnorderedOpen',
  'hidrpcActiveGeneration', 'hidrpcHandshakeComplete', 'hidrpcState',
  'rpcEvents', 'hidrpcEventsAccepted', 'hidrpcEventsDropped',
  'hidrpcInactiveMessages', 'usbHid',
  'macroActive', 'macroStateRevision', 'macroAccepted', 'macroCancelled',
  'absoluteMoveBackpressureDropped', 'releaseRequests', 'releasePending',
  'releaseRetries', 'releaseFailures', 'usbReportRetries',
]) {
  if (!httpSource.includes(required)) {
    throw new Error(`missing JetKVM transport stats token: ${required}`);
  }
}
for (const required of [
  'JETKVM_HIDRPC_TYPE_HANDSHAKE', 'JETKVM_HIDRPC_VERSION',
  '{ "ping", rpc_ping }', '{ "getVideoState", rpc_get_video_state }',
  '{ "getLocalVersion", rpc_get_local_version }',
  '{ "getNetworkSettings", rpc_get_network_settings }',
  '{ "getAudioConfig", rpc_get_audio_config }',
  '{ "getStreamQualityFactor", rpc_get_stream_quality }',
  '{ "setStreamQualityFactor", rpc_set_stream_quality }',
  '{ "getVideoCodecPreference", rpc_get_video_codec }',
  '{ "setVideoCodecPreference", rpc_set_video_codec }',
  'dispatch.error_code = -32601',
]) {
  if (!controlProtocolSource.includes(required)) {
    throw new Error(`missing JetKVM control protocol token: ${required}`);
  }
}
const encodeOneBody = extractCFunction(captureSource, 'h264_pipeline_encode_one');
const encodeFailureIndex = encodeOneBody.indexOf(
  'if (hr != ESP_H264_ERR_OK || out_frame.length == 0)');
const syncFailureIndex = encodeOneBody.indexOf(
  'h264_sync_idr_production_complete(ctx, false)', encodeFailureIndex);
if (encodeFailureIndex < 0 || syncFailureIndex < encodeFailureIndex) {
  throw new Error('failed forced-IDR encode must requeue its transport token');
}
const syncSlotBody = extractCFunction(transportSource, 'slot_is_decodable_sync');
for (const required of [
  'slot->has_idr', 'slot->has_sps', 'slot->has_pps',
  'slot->sync_idr_generation == generation',
]) {
  if (!syncSlotBody.includes(required)) {
    throw new Error(`peer-start sync IDR contract is missing: ${required}`);
  }
}
for (const required of [
  '#define ESP_H264_REFERENCE_POOL_SIZE',
  'ESP_H264_MAX_WIDTH + H264_DMA_MACRO_SIZE - 1',
  'static uint8_t s_reference_pool[ESP_H264_REFERENCE_POOL_SIZE]',
  'static esp_h264_param_t *s_reference_pool_owner',
  '__atomic_compare_exchange_n(',
  'param->ref = s_reference_pool',
  'param->ref_uses_static_pool = true',
]) {
  if (!h264ParamSource.includes(required)) {
    throw new Error(`static maximum-width H264 reference pool is missing: ${required}`);
  }
}
if (!projectCmake.includes(
  'patches/esp_h264-1.4.0-max-reference-width.patch')) {
  throw new Error('clean builds must replay the maximum-width H264 reference patch');
}

for (const required of [
  'capture_timestamp_us', 'capture_eof_timestamp_us',
  'encode_overlapped_capture', 'ppa_done_timestamp_us',
  'encode_start_timestamp_us', 'encode_done_timestamp_us',
]) {
  if (!frameMetaHeader.includes(required)) {
    throw new Error(`missing encoded-frame telemetry field: ${required}`);
  }
}

for (const required of [
  'WEBRTC_PLAYOUT_DELAY_URI', 'WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD',
  'webrtc_playout_delay_prepare_offer',
  'webrtc_playout_delay_answer_accepts',
  'webrtc_playout_delay_rtp_plan', 'webrtc_playout_delay_rtp_write',
]) {
  if (!playoutDelayHeader.includes(required) || !playoutDelaySource.includes(required)) {
    throw new Error(`playout-delay SDP/RTP helper contract is missing: ${required}`);
  }
}
for (const required of [
  'RTP_EXTENSION_PROFILE_ONE_BYTE 0xBEDE',
  'direction_allows_sender', 'direction_allows_receiver',
  'line_starts_with(sdp, &line, "a=")',
  'output[payload_offset + 4] = (uint8_t)((extension_id << 4) | 2U)',
]) {
  if (!playoutDelaySource.includes(required)) {
    throw new Error(`playout-delay wire/SDP safety contract is missing: ${required}`);
  }
}
for (const required of [
  'active_playout_delay_requested', 'active_playout_delay_offered',
  'active_playout_delay_negotiated', 'active_playout_delay_generation',
  'session_playout_delay_session_id == session_id',
  'webrtc_playout_delay_prepare_offer',
  'webrtc_playout_delay_answer_accepts',
  'webrtc_playout_delay_rtp_plan', 'webrtc_playout_delay_rtp_write',
  'rtp_playout_delay_packets_stamped',
  'rtp_playout_delay_packets_bypassed',
  'rtp_playout_delay_transform_errors',
]) {
  if (!transportSource.includes(required)) {
    throw new Error(`sender playout-delay transport contract is missing: ${required}`);
  }
}
if (transportSource.includes('session_playout_delay_generation')) {
  throw new Error('sender playout-delay session policy must survive peer generation changes');
}
const inboundBody = extractCFunction(transportSource, 'process_inbound');
const sendAnswerIndex = inboundBody.indexOf('esp_peer_send_msg(peer, &message)');
const activateDelayIndex = inboundBody.indexOf(
  'ctx->active_playout_delay_negotiated =');
if (sendAnswerIndex < 0 || activateDelayIndex < sendAnswerIndex ||
    !inboundBody.includes('ret != ESP_PEER_ERR_NONE')) {
  throw new Error('playout-delay negotiation may activate only after esp_peer accepts SDP');
}
const submitBody = extractCFunction(
  transportSource, 'webrtc_transport_session_submit');
if (submitBody.includes('webrtc_playout_delay_answer_accepts')) {
  throw new Error('HTTP task must not mutate active-peer playout-delay negotiation');
}
const closeMatchingBody = extractCFunction(transportSource, 'close_session_matching');
for (const required of [
  'generation == ctx->generation',
  'ctx->session_active = false',
  'next_generation_locked(ctx)',
  'ctx->rebuild_requested = true',
]) {
  if (!closeMatchingBody.includes(required)) {
    throw new Error(`atomic session-close contract is missing: ${required}`);
  }
}
if (!submitBody.includes(
  'close_session_matching(ctx, session_id, generation, true)')) {
  throw new Error('stale-generation BYE must not close the current session');
}
const pollBody = extractCFunction(
  transportSource, 'session_poll_with_role');
for (const required of [
  'next->session_id == session_id',
  'next->generation == ctx->generation',
  'ctx->outbound_count--',
]) {
  if (!pollBody.includes(required)) {
    throw new Error(`signaling poll generation filter is missing: ${required}`);
  }
}
if (pollBody.includes('while (outbound_pop(ctx')) {
  throw new Error('session poll must validate and pop signaling under one lock');
}
const peerTaskBody = extractCFunction(transportSource, 'peer_task');
for (const required of [
  '!ctx->peer && !ctx->rebuild_requested',
  'session_id = ctx->session_id',
  'generation = ctx->generation',
  'width = ctx->width', 'height = ctx->height', 'fps = ctx->fps',
]) {
  if (!peerTaskBody.includes(required)) {
    throw new Error(`post-teardown peer-open snapshot is missing: ${required}`);
  }
}
const openPeerBody = extractCFunction(transportSource, 'open_peer');
for (const required of [
  'ctx->session_id == session_id', 'ctx->generation == generation',
  '!ctx->mode_teardown_waiters',
  'return ESP_ERR_INVALID_STATE',
]) {
  if (!openPeerBody.includes(required)) {
    throw new Error(`peer-open generation guard is missing: ${required}`);
  }
}
for (const required of [
  'senderPlayoutDelayRequested', '"rtpPlayoutDelay"',
  '"packetsStamped"', '"packetsBypassed"', '"transformErrors"',
]) {
  if (!httpSource.includes(required)) {
    throw new Error(`sender playout-delay HTTP evidence is missing: ${required}`);
  }
}
for (const required of [
  '.ppa_done_timestamp_us = 0',
  '.encode_start_timestamp_us = start_us',
  '.encode_done_timestamp_us = end_us',
  'direct_h264_pipeline_submit(',
  'job.csi_dma_eof_timestamp_us',
  '"direct"',
]) {
  if (!captureSource.includes(required)) {
    throw new Error(`capture/encode telemetry handoff is missing: ${required}`);
  }
}
for (const required of [
  'webrtc_transport_stage_age_stats_t',
  'webrtc_transport_stage_age_summary_t',
  'capture_to_ppa_done', 'capture_to_encode_start', 'capture_to_encode_done',
  'capture_to_au_copy_ready', 'capture_to_peer_take',
  'capture_to_send_video_return', 'capture_to_first_rtp',
  'capture_to_rtp_marker', 'capture_to_srtp_first',
  'capture_to_srtp_marker', 'capture_to_udp_send_first',
  'capture_to_udp_send_marker', 'capture_to_emac_handoff_first',
  'capture_to_emac_handoff_marker', 'network_timing',
]) {
  if (!publicHeader.includes(required)) {
    throw new Error(`public stage-age snapshot is missing: ${required}`);
  }
}
for (const required of [
  'NETWORK_TRACE_SLOTS', 'webrtc_network_timing_begin',
  'webrtc_network_timing_cancel', 'webrtc_network_timing_record_rtp',
  'webrtc_network_timing_record_ethernet', 'ethernet_udp_payload',
  'capture_to_srtp_first', 'capture_to_udp_send_first',
  'capture_to_emac_handoff_first', 'trace_overwrites',
]) {
  if (!networkTimingSource.includes(required)) {
    throw new Error(`network timing implementation is missing: ${required}`);
  }
}
for (const required of [
  '__wrap_srtp_protect', '__real_srtp_protect',
  '__wrap_srtp_cipher_encrypt', '__real_srtp_cipher_encrypt',
  '__wrap_lwip_sendto', '__real_lwip_sendto',
  '__wrap_esp_eth_transmit', '__real_esp_eth_transmit',
  'esp_timer_get_time()', 'webrtc_network_timing_wrappers_link_anchor',
]) {
  if (!networkTimingWrappers.includes(required)) {
    throw new Error(`network timing wrapper is missing: ${required}`);
  }
}
for (const required of [
  'STAGE_AGE_MAX_US', 'record_stage_age_locked',
  'UINT64_MAX - accumulator->total_us < age_us',
  'int64_t au_copy_ready_us = trace_available ? esp_timer_get_time() : 0',
  'int64_t peer_take_us = esp_timer_get_time()',
  '.expected_rtp_timestamp = (slot->pts_90khz / 90U) * 90U',
  'timestamp == trace->expected_rtp_timestamp',
  'int64_t send_video_return_us = esp_timer_get_time()',
  'aggregate_active_trace_locked(ctx, send_video_return_us)',
  'mode_identity_changed = ctx->stats.stage_age.epoch == 0',
]) {
  if (!transportSource.includes(required)) {
    throw new Error(`stage-age implementation contract is missing: ${required}`);
  }
}

const peerCloseHelper = extractCFunction(
  transportSource, 'close_peer_handle_and_flush_outbound');
const peerCloseIndex = peerCloseHelper.indexOf('esp_peer_close(peer)');
const outboundFlushIndex = peerCloseHelper.indexOf('flush_outbound(ctx)');
if (peerCloseHelper.includes('esp_peer_disconnect(peer)')) {
  throw new Error(
    'esp_peer_close already owns vendor disconnect; explicit disconnect corrupts reconnect teardown');
}
if (peerCloseIndex < 0 || outboundFlushIndex < peerCloseIndex) {
  throw new Error('peer close must flush heap-backed outbound signaling after esp_peer_close');
}
if (!peerCloseHelper.includes('ret == ESP_PEER_ERR_NONE') ||
    !peerCloseHelper.includes('esp_peer_close returned %d')) {
  throw new Error('peer close must inspect and log the esp_peer_close return value');
}
if ((transportSource.match(/esp_peer_close\s*\(/g) || []).length !== 1) {
  throw new Error('all esp_peer_close calls must use the close-and-flush helper');
}
if ((transportSource.match(/esp_peer_disconnect\s*\(/g) || []).length !== 0) {
  throw new Error('transport must not disconnect the vendor graph twice');
}
const closePeerBody = extractCFunction(transportSource, 'close_peer');
for (const required of [
  'ctx->peer = NULL',
  'ctx->active_peer_session = 0',
  'ctx->active_peer_generation = 0',
  'close_peer_handle_and_flush_outbound(ctx, peer, "peer teardown")',
]) {
  if (!closePeerBody.includes(required)) {
    throw new Error(`peer teardown contract is missing: ${required}`);
  }
}
if (!transportSource.includes(
  'close_peer_handle_and_flush_outbound(ctx, peer, "peer open rollback")')) {
  throw new Error('failed peer open must use the close-and-flush rollback path');
}
const peerReleasedBody = extractCFunction(transportSource, 'peer_released_locked');
if (!peerReleasedBody.includes('return ctx->outbound_count == 0 &&')) {
  throw new Error('peer release cannot complete with queued outbound signaling');
}
const copyingSubmitBody = extractCFunction(
  transportSource, 'webrtc_transport_send_annexb_traced');
const finishAccessUnitBody = extractCFunction(transportSource, 'finish_access_unit');
const copyIndexForStageAge = copyingSubmitBody.indexOf(
  'memcpy(ctx->slots[slot_index].data, data, size)');
const finishCopyIndex = copyingSubmitBody.indexOf('return finish_access_unit(');
const copyReadyIndex = finishAccessUnitBody.indexOf(
  'int64_t au_copy_ready_us = trace_available ? esp_timer_get_time() : 0');
if (copyIndexForStageAge < 0 || finishCopyIndex < copyIndexForStageAge ||
    copyReadyIndex < 0 ||
    finishAccessUnitBody.indexOf('slot->state = AU_SLOT_READY') < copyReadyIndex) {
  throw new Error('AU-ready timestamp must follow any payload copy and precede publication');
}
const sendVideoIndex = transportSource.indexOf('esp_peer_send_video(ctx->peer, &frame)');
const sendReturnIndex = transportSource.indexOf(
  'int64_t send_video_return_us = esp_timer_get_time()');
if (sendVideoIndex < 0 || sendReturnIndex < sendVideoIndex) {
  throw new Error('send-video-return timestamp must be sampled after esp_peer_send_video');
}
for (const required of [
  '"stageAge"', '"unit", "us"', '"clock", "esp_timer"',
  '"origin", "csi-eof"', '"cohort"',
  '"rtpObservationPoint"', 'sender-rtp-transformer+srtp+lwip+emac-handoff',
  '"networkEgressMeasured", true', '"wireEgressMeasured", false', '"epoch"',
  '"captureToPpaDone"', '"captureToEncodeStart"',
  '"captureToEncodeDone"', '"captureToAuCopyReady"',
  '"captureToPeerTake"', '"captureToSendVideoReturn"',
  '"captureToFirstRtp"', '"captureToRtpMarker"',
  '"captureToSrtpFirst"', '"captureToSrtpMarker"',
  '"captureToUdpSendFirst"', '"captureToUdpSendMarker"',
  '"captureToEmacHandoffFirst"', '"captureToEmacHandoffMarker"',
  '"networkTiming"', '"srtpProtect"', '"lwipSendto"',
  '"srtpCipher"', '"srtpAuthAndFramework"',
  '"emacHandoff"', '"wireCompletionMeasured", false',
  '"count"', '"avgUs"', '"maxUs"',
  '"invalidTimestamps"', '"orderErrors"',
  '"accumulatorOverflows"', '"epochMismatches"',
  '"missingFirstRtp"', '"missingRtpMarker"',
]) {
  if (!httpSource.includes(required)) {
    throw new Error(`stage-age HTTP contract is missing: ${required}`);
  }
}

const callbackStart = html.indexOf('const onFrame = (callbackNow, metadata) =>');
const callbackEnd = html.indexOf(
  '\n    };\n    state.rvfcId = video.requestVideoFrameCallback(onFrame);', callbackStart);
if (callbackStart < 0 || callbackEnd < 0) throw new Error('frame callback body not found');
const callbackBody = html.slice(callbackStart, callbackEnd);
const rearmIndex = callbackBody.indexOf('video.requestVideoFrameCallback(onFrame)');
const ledgerIndex = callbackBody.indexOf('observePresentationLedger(callbackNow, metadata)');
const videoFrameIndex = callbackBody.indexOf('new VideoFrame(video)');
const copyIndex = callbackBody.indexOf('frame.copyTo');
if (rearmIndex < 0 || videoFrameIndex < 0 || rearmIndex > videoFrameIndex) {
  throw new Error('rVFC must be rearmed before source-identity extraction');
}
if (ledgerIndex < rearmIndex || ledgerIndex > videoFrameIndex) {
  throw new Error('the presentation ledger must run after rearming rVFC and before VideoFrame identity work');
}
if (copyIndex < videoFrameIndex) {
  throw new Error('Gray-code identity must come from the callback VideoFrame');
}
if ((callbackBody.match(/frame\.copyTo\(/g) || []).length !== 1 ||
    !callbackBody.includes('telemetryRow.pixels') ||
    !callbackBody.includes('decodeTelemetryRow')) {
  throw new Error('rVFC must decode all telemetry from exactly one packed scanline copy');
}
if (callbackBody.includes('Promise.all(')) {
  throw new Error('parallel per-word VideoFrame copies are forbidden');
}
if (callbackBody.includes("$('fingerprintText').textContent")) {
  throw new Error('per-frame DOM updates must stay out of the rVFC hot path');
}
if (callbackBody.includes('fingerprintContext.drawImage(video') ||
    callbackBody.includes('fingerprintContext.getImageData')) {
  throw new Error('full-frame synchronous canvas readback is forbidden in rVFC');
}
if ((callbackBody.match(/video\.requestVideoFrameCallback\(onFrame\)/g) || []).length !== 1) {
  throw new Error('each rVFC callback must rearm exactly one successor');
}
if (callbackBody.includes('fp.count++') || callbackBody.includes('fp.duplicateCount++')) {
  throw new Error('an rVFC callback cannot count until Gray-code extraction succeeds');
}
if (!callbackBody.includes('metadata.presentedFrames') ||
    callbackBody.includes('fp.count +=') || callbackBody.includes('fp.count = presentedFrames')) {
  throw new Error('presentedFrames may diagnose callback skips but must not inflate render count');
}

const drainStart = html.indexOf('function drainIdentityResults(epoch, identityTrackToken)');
const drainEnd = html.indexOf('\n  function recordIdentityResult(', drainStart);
if (drainStart < 0 || drainEnd < 0) throw new Error('identity result drain not found');
const drainBody = html.slice(drainStart, drainEnd);
const decodeIndex = drainBody.indexOf('grayToBinary(result.frameGray)');
const anomalyIndex = drainBody.indexOf('fp.identityDecodeAnomalies++');
const classifiedIndex = drainBody.indexOf('fp.count++');
const confidenceIndex = drainBody.indexOf('!result.telemetryConfidence?.confidencePass');
const integrityIndex = drainBody.indexOf('(result.wireCrc32c >>> 0) !== expectedClockCheck');
const observeLatencyIndex = drainBody.indexOf('observeLatency({...result, sourceClockMs32}, advance)');
if (decodeIndex < 0 || confidenceIndex < 0 || integrityIndex < confidenceIndex ||
    decodeIndex < integrityIndex || classifiedIndex < decodeIndex) {
  throw new Error('confidence and wire CRC must pass before Gray decode and classification');
}
if ((drainBody.match(/fp\.count\+\+/g) || []).length !== 1) {
  throw new Error('each classified identity result must count exactly once');
}
if (integrityIndex > decodeIndex || integrityIndex > classifiedIndex ||
    observeLatencyIndex < classifiedIndex) {
  throw new Error('CRC integrity must pass before identity classification or frame-age sampling');
}
const integrityFailureStart = drainBody.indexOf('if ((result.wireCrc32c >>> 0) !== expectedClockCheck)');
const integrityFailureEnd = drainBody.indexOf('\n      }', integrityFailureStart);
const integrityFailureBody = drainBody.slice(integrityFailureStart, integrityFailureEnd);
if (!integrityFailureBody.includes('fp.unclassifiedCount++') ||
    !integrityFailureBody.includes('continue;')) {
  throw new Error('CRC failure must become unclassified and fail closed');
}

const prepareRowStart = html.indexOf('function prepareTelemetryRowCopy(');
const prepareRowEnd = html.indexOf('\n  function drainIdentityResults(', prepareRowStart);
const prepareRowBody = html.slice(prepareRowStart, prepareRowEnd);
if (prepareRowStart < 0 || prepareRowEnd < 0 ||
    !prepareRowBody.includes('height:1') ||
    prepareRowBody.includes('drawImage') || prepareRowBody.includes('getImageData')) {
  throw new Error('telemetry extraction must remain an asynchronous one-scanline VideoFrame path');
}

const screenshotStart = html.indexOf('async function captureScreenshot()');
const screenshotEnd = html.indexOf('\n  async function snapshot(', screenshotStart);
const screenshotBody = html.slice(screenshotStart, screenshotEnd);
if (screenshotStart < 0 || screenshotEnd < 0 ||
    !screenshotBody.includes('frame = new VideoFrame(video)') ||
    !screenshotBody.includes('frame.copyTo(') ||
    !screenshotBody.includes('screenshotContext.drawImage(frame,') ||
    screenshotBody.includes('screenshotContext.drawImage(video,')) {
  throw new Error('screenshot pixels and packed telemetry must use the same immutable VideoFrame');
}
if (screenshotBody.includes('latency.latest') || screenshotBody.includes('state.latency') ||
    !screenshotBody.includes('screenshotFrameAgeMs = unsignedDelta32(') ||
    !screenshotBody.includes('receiverFrameSelectionBracket')) {
  throw new Error('screenshot frame age must be decoded from its own frame and selection bracket');
}
if (screenshotBody.indexOf('receivedTelemetryCrc32c !== expectedTelemetryCrc32c') >
      screenshotBody.indexOf('grayToBinary(decoded.frameGray)') ||
    !screenshotBody.includes("invalidReason = 'telemetryLowConfidence'")) {
  throw new Error('screenshot confidence and wire CRC must pass before Gray decode');
}

const invalidateStart = html.indexOf('function invalidateIdentityTrack()');
const invalidateEnd = html.indexOf('\n  function beginIdentityTrack()', invalidateStart);
if (invalidateStart < 0 || invalidateEnd < 0) {
  throw new Error('track-scoped identity invalidation is missing');
}
const invalidateBody = html.slice(invalidateStart, invalidateEnd);
for (const required of [
  'state.identityTrackToken++', 'state.identityResults.clear()',
  'fp.lastSourceFrameId = null', 'fp.lastPresentedFrames = null',
  'state.identityNextSequence = fp.callbackCount + 1',
]) {
  if (!invalidateBody.includes(required)) {
    throw new Error(`identity invalidation is missing ${required}`);
  }
}
if (/fp\.(?:count|duplicateCount|unclassifiedCount|callbackCount|compositorPresentedCount)\s*=/.test(
  invalidateBody)) {
  throw new Error('track replacement must not reset cumulative identity/render counters');
}
if ((callbackBody.match(/identityTrackToken !== state\.identityTrackToken/g) || []).length < 2) {
  throw new Error('both rVFC dispatch and async VideoFrame completion must reject stale tracks');
}
if (!callbackBody.includes(
  'recordIdentityResult(epoch, identityTrackToken, sequence, result)')) {
  throw new Error('identity result must retain its originating track token');
}
if (!invalidateBody.includes('state.latency.lastSourceClockMs32 = null')) {
  throw new Error('track replacement must reset the source-clock progression baseline');
}
if (/state\.latency\.(?:validSampleCount|invalidSampleCount)\s*=/.test(invalidateBody)) {
  throw new Error('track replacement must preserve cumulative latency samples');
}

function extractFunction(source, name) {
  const start = source.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`function ${name} not found`);
  const open = source.indexOf('{', start);
  let depth = 0;
  for (let index=open; index<source.length; index++) {
    if (source[index] === '{') depth++;
    if (source[index] === '}' && --depth === 0) return source.slice(start, index + 1);
  }
  throw new Error(`function ${name} is unterminated`);
}

const describeReceiverLatencyControl = runInNewContext(
  `(${extractFunction(match[1], 'describeReceiverLatencyControl')})`);
const describeReceiverLatencyExperiment = runInNewContext(
  `(${extractFunction(match[1], 'describeReceiverLatencyExperiment')})`);
const describeDisplayProfile = runInNewContext(
  `(${extractFunction(match[1], 'describeDisplayProfile')})`);
const dashboardProfile = describeDisplayProfile(null);
assert.equal(dashboardProfile.effectiveMode, 'dashboard');
assert.equal(dashboardProfile.bare, false);
assert.equal(dashboardProfile.videoOnly, false);
assert.equal(dashboardProfile.domTelemetryUpdatesEnabled, true);
assert.equal(dashboardProfile.continuousLocalClockRafEnabled, true);
const bareProfile = describeDisplayProfile('bare');
assert.equal(bareProfile.effectiveMode, 'bare');
assert.equal(bareProfile.bare, true);
assert.equal(bareProfile.videoOnly, true);
assert.equal(bareProfile.fillViewport, false);
assert.equal(bareProfile.dashboardLayoutPreservedForAb, true);
assert.equal(bareProfile.domTelemetryUpdatesEnabled, false);
assert.equal(bareProfile.continuousLocalClockRafEnabled, false);
const bareFillProfile = describeDisplayProfile('bare-fill');
assert.equal(bareFillProfile.effectiveMode, 'bare-fill');
assert.equal(bareFillProfile.bare, true);
assert.equal(bareFillProfile.videoOnly, true);
assert.equal(bareFillProfile.fillViewport, true);
assert.equal(bareFillProfile.dashboardLayoutPreservedForAb, false);
assert.equal(bareFillProfile.domTelemetryUpdatesEnabled, false);
assert.equal(bareFillProfile.continuousLocalClockRafEnabled, false);
assert.equal(describeDisplayProfile('unknown').effectiveMode, 'dashboard');
const defaultExperiment = describeReceiverLatencyExperiment(null);
assert.equal(defaultExperiment.queryValue, null);
assert.equal(defaultExperiment.defaultMode, 'jitter+playout');
assert.equal(defaultExperiment.effectiveMode, '1');
assert.equal(defaultExperiment.productionDefaultApplied, true);
assert.equal(defaultExperiment.explicitOptOut, false);
assert.equal(defaultExperiment.experiment, 'receiver-zero-delay-hints');
assert.equal(defaultExperiment.requested, true);
assert.equal(defaultExperiment.jitterBufferTargetRequested, true);
assert.equal(defaultExperiment.jitterBufferTargetRequestedValue, 0);
assert.equal(defaultExperiment.playoutDelayHintRequested, true);
assert.equal(defaultExperiment.playoutDelayHintRequestedValue, 0);
for (const queryValue of ['0', 'off']) {
  const config = describeReceiverLatencyExperiment(queryValue);
  assert.equal(config.defaultMode, 'jitter+playout');
  assert.equal(config.effectiveMode, 'browser-default');
  assert.equal(config.productionDefaultApplied, false);
  assert.equal(config.explicitOptOut, true);
  assert.equal(config.experiment, 'browser-default');
  assert.equal(config.requested, false);
  assert.equal(config.jitterBufferTargetRequested, false);
  assert.equal(config.jitterBufferTargetRequestedValue, null);
  assert.equal(config.playoutDelayHintRequested, false);
  assert.equal(config.playoutDelayHintRequestedValue, null);
}
const fallbackExperiment = describeReceiverLatencyExperiment('unsupported-value');
assert.equal(fallbackExperiment.effectiveMode, '1');
assert.equal(fallbackExperiment.productionDefaultApplied, true);
assert.equal(fallbackExperiment.explicitOptOut, false);
assert.equal(fallbackExperiment.playoutDelayHintRequested, true);
assert.equal(fallbackExperiment.jitterBufferTargetRequested, true);
const dualExperiment = describeReceiverLatencyExperiment('1');
assert.equal(dualExperiment.effectiveMode, '1');
assert.equal(dualExperiment.productionDefaultApplied, false);
assert.equal(dualExperiment.explicitOptOut, false);
assert.equal(dualExperiment.experiment, 'receiver-zero-delay-hints');
assert.equal(dualExperiment.requested, true);
assert.equal(dualExperiment.jitterBufferTargetRequested, true);
assert.equal(dualExperiment.jitterBufferTargetRequestedValue, 0);
assert.equal(dualExperiment.playoutDelayHintRequested, true);
assert.equal(dualExperiment.playoutDelayHintRequestedValue, 0);
const jitterExperiment = describeReceiverLatencyExperiment('jitter');
assert.equal(jitterExperiment.experiment, 'receiver-zero-jitter-buffer-target');
assert.equal(jitterExperiment.jitterBufferTargetRequested, true);
assert.equal(jitterExperiment.jitterBufferTargetRequestedValue, 0);
assert.equal(jitterExperiment.playoutDelayHintRequested, false);
assert.equal(jitterExperiment.playoutDelayHintRequestedValue, null);
const playoutExperiment = describeReceiverLatencyExperiment('playout');
assert.equal(playoutExperiment.experiment, 'receiver-zero-playout-delay-hint');
assert.equal(playoutExperiment.jitterBufferTargetRequested, false);
assert.equal(playoutExperiment.jitterBufferTargetRequestedValue, null);
assert.equal(playoutExperiment.playoutDelayHintRequested, true);
assert.equal(playoutExperiment.playoutDelayHintRequestedValue, 0);

let optOutJitterWrites = 0;
let optOutPlayoutWrites = 0;
const optOutReceiver = {};
Object.defineProperties(optOutReceiver, {
  jitterBufferTarget:{
    get:() => 17,
    set:() => { optOutJitterWrites++; },
  },
  playoutDelayHint:{
    get:() => 0.25,
    set:() => { optOutPlayoutWrites++; },
  },
});
const optOutLatencyControl = describeReceiverLatencyControl(optOutReceiver, false, false);
assert.equal(optOutJitterWrites, 0,
  'lowLatency=0/off must not write jitterBufferTarget');
assert.equal(optOutPlayoutWrites, 0,
  'lowLatency=0/off must not write playoutDelayHint');
assert.equal(optOutLatencyControl.experiment, 'browser-default');
assert.equal(optOutLatencyControl.requested, false);
assert.equal(optOutLatencyControl.jitterBufferTargetRequested, false);
assert.equal(optOutLatencyControl.jitterBufferTargetRequestedValue, null);
assert.equal(optOutLatencyControl.playoutDelayHintRequested, false);
assert.equal(optOutLatencyControl.playoutDelayHintRequestedValue, null);
assert.equal(optOutLatencyControl.jitterBufferTargetSupported, true);
assert.equal(optOutLatencyControl.playoutDelayHintSupported, true);
assert.equal(optOutLatencyControl.jitterBufferTargetReadback, 17);
assert.equal(optOutLatencyControl.playoutDelayHintReadback, 0.25);
assert.equal(optOutLatencyControl.jitterBufferTargetUntouched, true);
assert.equal(optOutLatencyControl.playoutDelayHintUntouched, true);
assert.equal(optOutLatencyControl.mutated, false);
assert.deepEqual(Array.from(optOutLatencyControl.errors), []);

const lowLatencyReceiver = {jitterBufferTarget:17, playoutDelayHint:0.25};
const lowLatencyControl = describeReceiverLatencyControl(lowLatencyReceiver, true, true);
assert.equal(lowLatencyReceiver.jitterBufferTarget, 0);
assert.equal(lowLatencyReceiver.playoutDelayHint, 0);
assert.equal(lowLatencyControl.experiment, 'receiver-zero-delay-hints');
assert.equal(lowLatencyControl.requested, true);
assert.equal(lowLatencyControl.jitterBufferTargetRequested, true);
assert.equal(lowLatencyControl.jitterBufferTargetRequestedValue, 0);
assert.equal(lowLatencyControl.playoutDelayHintRequested, true);
assert.equal(lowLatencyControl.playoutDelayHintRequestedValue, 0);
assert.equal(lowLatencyControl.jitterBufferTargetMutationAttempted, true);
assert.equal(lowLatencyControl.playoutDelayHintMutationAttempted, true);
assert.equal(lowLatencyControl.jitterBufferTargetMutated, true);
assert.equal(lowLatencyControl.playoutDelayHintMutated, true);
assert.equal(lowLatencyControl.jitterBufferTargetReadback, 0);
assert.equal(lowLatencyControl.playoutDelayHintReadback, 0);
assert.equal(lowLatencyControl.jitterBufferTargetUntouched, false);
assert.equal(lowLatencyControl.playoutDelayHintUntouched, false);
assert.equal(lowLatencyControl.mutated, true);
assert.deepEqual(Array.from(lowLatencyControl.errors), []);

let jitterOnlyValue = 17;
let jitterOnlyPlayoutValue = 0.25;
let jitterOnlyWrites = 0;
let jitterOnlyPlayoutWrites = 0;
const jitterOnlyReceiver = {};
Object.defineProperties(jitterOnlyReceiver, {
  jitterBufferTarget:{
    get:() => jitterOnlyValue,
    set:value => { jitterOnlyWrites++; jitterOnlyValue = value; },
  },
  playoutDelayHint:{
    get:() => jitterOnlyPlayoutValue,
    set:value => { jitterOnlyPlayoutWrites++; jitterOnlyPlayoutValue = value; },
  },
});
const jitterOnlyControl = describeReceiverLatencyControl(
  jitterOnlyReceiver, true, false);
assert.equal(jitterOnlyWrites, 1);
assert.equal(jitterOnlyPlayoutWrites, 0,
  'lowLatency=jitter must not write playoutDelayHint');
assert.equal(jitterOnlyValue, 0);
assert.equal(jitterOnlyPlayoutValue, 0.25);
assert.equal(jitterOnlyControl.experiment, 'receiver-zero-jitter-buffer-target');
assert.equal(jitterOnlyControl.requested, true);
assert.equal(jitterOnlyControl.jitterBufferTargetRequested, true);
assert.equal(jitterOnlyControl.jitterBufferTargetRequestedValue, 0);
assert.equal(jitterOnlyControl.jitterBufferTargetMutationAttempted, true);
assert.equal(jitterOnlyControl.playoutDelayHintRequested, false);
assert.equal(jitterOnlyControl.playoutDelayHintRequestedValue, null);
assert.equal(jitterOnlyControl.playoutDelayHintMutationAttempted, false);
assert.equal(jitterOnlyControl.jitterBufferTargetReadback, 0);
assert.equal(jitterOnlyControl.playoutDelayHintReadback, 0.25);

let playoutOnlyJitterValue = 17;
let playoutOnlyValue = 0.25;
let playoutOnlyJitterWrites = 0;
let playoutOnlyWrites = 0;
const playoutOnlyReceiver = {};
Object.defineProperties(playoutOnlyReceiver, {
  jitterBufferTarget:{
    get:() => playoutOnlyJitterValue,
    set:value => { playoutOnlyJitterWrites++; playoutOnlyJitterValue = value; },
  },
  playoutDelayHint:{
    get:() => playoutOnlyValue,
    set:value => { playoutOnlyWrites++; playoutOnlyValue = value; },
  },
});
const playoutOnlyControl = describeReceiverLatencyControl(
  playoutOnlyReceiver, false, true);
assert.equal(playoutOnlyJitterWrites, 0,
  'lowLatency=playout must not write jitterBufferTarget');
assert.equal(playoutOnlyWrites, 1);
assert.equal(playoutOnlyJitterValue, 17);
assert.equal(playoutOnlyValue, 0);
assert.equal(playoutOnlyControl.experiment, 'receiver-zero-playout-delay-hint');
assert.equal(playoutOnlyControl.requested, true);
assert.equal(playoutOnlyControl.jitterBufferTargetRequested, false);
assert.equal(playoutOnlyControl.jitterBufferTargetRequestedValue, null);
assert.equal(playoutOnlyControl.jitterBufferTargetMutationAttempted, false);
assert.equal(playoutOnlyControl.playoutDelayHintRequested, true);
assert.equal(playoutOnlyControl.playoutDelayHintRequestedValue, 0);
assert.equal(playoutOnlyControl.playoutDelayHintMutationAttempted, true);
assert.equal(playoutOnlyControl.jitterBufferTargetReadback, 17);
assert.equal(playoutOnlyControl.playoutDelayHintReadback, 0);

let independentPlayoutWrite = null;
const partiallyWritableReceiver = {};
Object.defineProperties(partiallyWritableReceiver, {
  jitterBufferTarget:{
    get:() => 19,
    set:() => { throw new Error('jitter write denied'); },
  },
  playoutDelayHint:{
    get:() => { throw new Error('playout read denied'); },
    set:value => { independentPlayoutWrite = value; },
  },
});
const partialControl = describeReceiverLatencyControl(partiallyWritableReceiver, true, true);
assert.equal(independentPlayoutWrite, 0,
  'a jitterBufferTarget failure must not prevent the playoutDelayHint attempt');
assert.equal(partialControl.jitterBufferTargetMutated, false);
assert.equal(partialControl.playoutDelayHintMutated, true);
assert.equal(partialControl.jitterBufferTargetReadback, 19);
assert.equal(partialControl.playoutDelayHintReadback, null);
assert.equal(partialControl.mutated, true);
assert.deepEqual(Array.from(partialControl.errors), [
  'jitterBufferTarget write: jitter write denied',
  'playoutDelayHint readback: playout read denied',
]);

const unsupportedControl = describeReceiverLatencyControl({}, true, true);
assert.equal(unsupportedControl.jitterBufferTargetSupported, false);
assert.equal(unsupportedControl.playoutDelayHintSupported, false);
assert.equal(unsupportedControl.jitterBufferTargetMutationAttempted, false);
assert.equal(unsupportedControl.playoutDelayHintMutationAttempted, false);
assert.equal(unsupportedControl.mutated, false);
assert.deepEqual(Array.from(unsupportedControl.errors), []);

const unsignedDelta32 = runInNewContext(
  `(${extractFunction(match[1], 'unsignedDelta32')})`);
assert.equal(unsignedDelta32(5, 0xffff_fffb), 10,
  'modulo subtraction must survive a uint32 wrap');
assert.equal(unsignedDelta32(120, 20), 100);

const histogramPercentile = runInNewContext(
  `(${extractFunction(match[1], 'histogramPercentile')})`);
const latencyHistogram = new Uint32Array(201);
latencyHistogram[40] = 1;
latencyHistogram[80] = 2;
latencyHistogram[120] = 1;
latencyHistogram[200] = 1;
assert.equal(histogramPercentile(latencyHistogram, 5, 0.50), 80);
assert.equal(histogramPercentile(latencyHistogram, 5, 0.95), 200);
assert.equal(histogramPercentile(latencyHistogram, 5, 0.99), 200);
assert.equal(histogramPercentile(latencyHistogram, 0, 0.50), null);

const sparseHistogramSummary = runInNewContext(
  `(${extractFunction(match[1], 'sparseHistogramSummary')})`);
const sparseSummary = sparseHistogramSummary(latencyHistogram, 5);
assert.deepEqual(Array.from(sparseSummary.histogramMs, bin => Array.from(bin)),
  [[40, 1], [80, 2], [120, 1], [200, 1]]);
assert.equal(sparseSummary.histogramSampleCount, 5);
assert.equal(sparseSummary.histogramWeightedSumMs, 520);
assert.equal(sparseSummary.p50Ms, 80);
assert.equal(sparseSummary.p95Ms, 200);
assert.equal(sparseSummary.p99Ms, 200);
assert.equal(sparseSummary.histogramMs.reduce((sum, [, count]) => sum + count, 0), 5,
  'sparse histogram bin total must equal validSampleCount');

const latencySummaryHarness = runInNewContext(`
  const LATENCY_METHOD = {version:'test'};
  const telemetryFps = 30;
  const performance = {timeOrigin:123};
  const round = value => Number(value.toFixed(2));
  const histogram = new Uint32Array(201);
  histogram[40] = 1; histogram[80] = 2; histogram[120] = 1; histogram[200] = 1;
  const state = {latency:{validSampleCount:5, invalidSampleCount:2,
    invalidReasons:{timecodeIntegrity:2}, totalMs:520, minMs:40, maxMs:200,
    latestMs:80, latestSourceClockMs32:1, latestReceiverClockMs32:81,
    latestSourceEpochMs:1, latestReceiverEpochMs:81, sourceClockWraps:0,
    trackResets:1, histogram}};
  ${extractFunction(match[1], 'sparseHistogramSummary')}
  ${extractFunction(match[1], 'latencySummary')}
  latencySummary();
`);
assert.equal(latencySummaryHarness.histogramSampleCount,
  latencySummaryHarness.validSampleCount);
assert.equal(latencySummaryHarness.histogramWeightedSumMs,
  latencySummaryHarness.totalMs);
assert.equal(latencySummaryHarness.histogramCountMatches, true);
assert.equal(latencySummaryHarness.histogramSumMatches, true);
assert.deepEqual(Array.from(latencySummaryHarness.histogramMs, bin => Array.from(bin)),
  [[40, 1], [80, 2], [120, 1], [200, 1]]);

const browserClockSample = runInNewContext(`
  const LATENCY_METHOD = {clockDomain:'test-domain'};
  const performance = {timeOrigin:1000.5, now:() => 25.25};
  const Date = {now:() => 1026};
  (${extractFunction(match[1], 'browserClockSample')})
`);
assert.deepEqual({...browserClockSample()}, {
  performanceTimeOrigin:1000.5, performanceNow:25.25, epochMs:1025.75,
  dateNowMs:1026, epochMinusDateMs:-0.25, clockMs32:1025,
  clockDomain:'test-domain',
});

const crc32cWords = runInNewContext(`
  (${extractFunction(match[1], 'crc32cWords')})
`);
assert.equal(crc32cWords([0x31323334, 0x35363738, 0x39000000]), 0xc48fc8d7,
  'word CRC sanity vector must remain stable');
const telemetryCrcHarness = runInNewContext(`
  const TIMECODE_MAGIC = 0x4b564d32;
  let telemetryFps = 30;
  ${extractFunction(match[1], 'crc32cWords')}
  ${extractFunction(match[1], 'telemetryModeWord')}
  ${extractFunction(match[1], 'telemetryCrc32c')}
  ({telemetryCrc32c, setFps(value) { telemetryFps = value; }});
`);
assert.equal(telemetryCrcHarness.telemetryCrc32c(1920, 1080, 1, 0x80000004),
  0x3189272d,
  'CRC-32C must match an independently generated packed-wire test vector');
telemetryCrcHarness.setFps(60);
assert.equal(telemetryCrcHarness.telemetryCrc32c(1920, 1080, 1, 0x80000004),
  0x360fdaf2,
  '60 FPS CRC-32C must match an independently generated packed-wire test vector');

const modeContract = runInNewContext(`
  (${extractFunction(match[1], 'modeContract')})
`);
assert.equal(modeContract(1920, 1080, 30).name, '1080p30');
assert.equal(modeContract(1280, 720, 30).name, '720p30');
assert.equal(modeContract(1280, 720, 60).name, '720p60');
assert.equal(modeContract(1280, 720, 0).name, 'unsupported');

const corruptTelemetryHarness = runInNewContext(`
  const TIMECODE_MAGIC = 0x4b564d32;
  const telemetryFps = 30;
  const state = {
    running:true, runEpoch:7, identityTrackToken:3, identityNextSequence:1,
    identityResults:new Map(),
    fingerprint:{count:0, duplicateCount:0, currentRun:0, longestRun:0,
      lastSourceFrameId:77, sourceFrameIdGaps:0, duplicateMediaTimeCount:0,
      lastMediaTime:0, lastHash:null, lastHashHex:null, lastGrayCode:null,
      identityFailures:0, identityDecodeAnomalies:0, unclassifiedCount:0},
    latency:{lastSourceClockMs32:1234},
  };
  const rejected = [];
  let observed = 0;
  function rejectLatency(reason) { rejected.push(reason); }
  function observeLatency() { observed++; }
  ${extractFunction(match[1], 'grayToBinary')}
  ${extractFunction(match[1], 'crc32cWords')}
  ${extractFunction(match[1], 'telemetryModeWord')}
  ${extractFunction(match[1], 'telemetryCrc32c')}
  ${extractFunction(match[1], 'drainIdentityResults')}
  const frameGray = 0x00000041;
  const clockGray = 0x0000079e;
  const validCrc = telemetryCrc32c(1920, 1080, frameGray, clockGray);
  state.identityResults.set(1, {frameGray:frameGray ^ 1, clockGray,
    wireCrc32c:validCrc, width:1920, height:1080,
    telemetryConfidence:{confidencePass:true}, mediaTime:1});
  drainIdentityResults(7, 3);
  ({count:state.fingerprint.count,
    duplicateCount:state.fingerprint.duplicateCount,
    unclassifiedCount:state.fingerprint.unclassifiedCount,
    lastSourceFrameId:state.fingerprint.lastSourceFrameId,
    lastSourceClockMs32:state.latency.lastSourceClockMs32,
    observed, rejected});
`);
assert.deepEqual({...corruptTelemetryHarness, rejected:Array.from(corruptTelemetryHarness.rejected)}, {
  count:0, duplicateCount:0, unclassifiedCount:1,
  lastSourceFrameId:77, lastSourceClockMs32:1234,
  observed:0, rejected:['timecodeIntegrity'],
}, 'a corrupt packed row must not manufacture classified/unique progress or move baselines');

const latencyHarness = runInNewContext(`
  const MAX_VALID_LATENCY_MS = 60000;
  const MAX_SOURCE_CLOCK_ADVANCE_MS = 10000;
  const state = {latency:{
    validSampleCount:0, invalidSampleCount:0,
    invalidReasons:{missingExpectedDisplayTime:0, sourceClockProgression:0,
      frameClockMismatch:0, outsideValidWindow:0},
    totalMs:0, minMs:null, maxMs:null, latestMs:null,
    latestSourceClockMs32:null, latestReceiverClockMs32:null,
    latestSourceEpochMs:null, latestReceiverEpochMs:null,
    lastSourceClockMs32:null, sourceClockWraps:0,
    histogram:new Uint32Array(MAX_VALID_LATENCY_MS + 1),
  }};
  function scheduleLatencyOverlayUpdate() {}
  ${extractFunction(match[1], 'unsignedDelta32')}
  ${extractFunction(match[1], 'rejectLatency')}
  ${extractFunction(match[1], 'observeLatency')}
  observeLatency({sourceClockMs32:0xfffffff0, receiverClockMs32:0x20,
    receiverEpochMs:4294967328}, 1);
  observeLatency({sourceClockMs32:0x10, receiverClockMs32:0x40,
    receiverEpochMs:4294967360}, 1);
  observeLatency({sourceClockMs32:0x30, receiverClockMs32:0x20000,
    receiverEpochMs:4295098368}, 1);
  ({validSampleCount:state.latency.validSampleCount,
    invalidSampleCount:state.latency.invalidSampleCount,
    outsideValidWindow:state.latency.invalidReasons.outsideValidWindow,
    minMs:state.latency.minMs, maxMs:state.latency.maxMs,
    wraps:state.latency.sourceClockWraps,
    at48:state.latency.histogram[48]});
`);
assert.deepEqual({...latencyHarness}, {
  validSampleCount:2, invalidSampleCount:1, outsideValidWindow:1,
  minMs:48, maxMs:48, wraps:1, at48:2,
});

const presentationLedgerHarness = runInNewContext(`
  const MAX_PRESENTATION_LEDGER_SAMPLES = 20000;
  const state = {activeTrack:{peerInstanceSequence:4}, peerInstanceSequence:4,
    presentationLedger:null};
  const round = (value, places=2) => Number.isFinite(value) ?
    Number(value.toFixed(places)) : 0;
  function finiteNumberOrNull(value) {
    const number = Number(value);
    return Number.isFinite(number) ? number : null;
  }
  ${extractFunction(match[1], 'createPresentationLedgerState')}
  ${extractFunction(match[1], 'finiteTimestamp')}
  ${extractFunction(match[1], 'observePresentationLedger')}
  state.presentationLedger = createPresentationLedgerState();
  observePresentationLedger(130, {receiveTime:100, presentationTime:120,
    expectedDisplayTime:133.333, processingDuration:0.006, captureTime:90,
    rtpTimestamp:1234, presentedFrames:7, mediaTime:1.25, width:1920, height:1080});
  observePresentationLedger(140, {});
  ({validSamples:state.presentationLedger.validSamples,
    invalidSamples:state.presentationLedger.invalidSamples,
    missingReceiveTime:state.presentationLedger.invalidReasons.missingReceiveTime,
    sample:state.presentationLedger.samples[0]});
`);
assert.equal(presentationLedgerHarness.validSamples, 1);
assert.equal(presentationLedgerHarness.invalidSamples, 1);
assert.equal(presentationLedgerHarness.missingReceiveTime, 1);
assert.deepEqual({...presentationLedgerHarness.sample}, {
  sequence:1, peerInstanceSequence:4, rtpTimestamp:1234, presentedFrames:7,
  mediaTime:1.25, width:1920, height:1080, callbackNowMs:130,
  captureTimeMs:90, receiveTimeMs:100, presentationTimeMs:120,
  expectedDisplayTimeMs:133.333, processingDurationMs:6,
  captureToReceiveMs:10, captureToExpectedDisplayMs:43.333,
  receiveToPresentationMs:20, nonDecodeResidualToPresentationMs:14,
  presentationToExpectedDisplayMs:13.333, receiveToExpectedDisplayMs:33.333,
  receiveToCallbackMs:30, callbackAfterPresentationMs:10,
  callbackToExpectedDisplayMs:3.333, valid:true,
});
const numericDistributionForTest = runInNewContext(
  `(${extractFunction(match[1], 'numericDistribution')})`,
  {round:(value, places=2) => Number(value.toFixed(places))});
assert.deepEqual({...numericDistributionForTest(
  [{missing:null}, {missing:undefined}], 'missing')}, {
  sampleCount:0, minMs:null, meanMs:null, p50Ms:null, p95Ms:null,
  p99Ms:null, maxMs:null,
}, 'missing optional rVFC metadata must not become a synthetic zero sample');

const closeStart = html.indexOf(
  "async function closePeer(reason='replace', expectedPeer=state.peer)");
const closeEnd = html.indexOf('\n  function requestReconnect(', closeStart);
const closeBody = html.slice(closeStart, closeEnd);
if (closeStart < 0 || closeEnd < 0 ||
    closeBody.indexOf('invalidateIdentityTrack()') > closeBody.indexOf('peer.close()')) {
  throw new Error('old track identity must be invalidated before its peer is closed');
}
if (!closeBody.includes('observeDecodedCounter(findVideoInbound(finalReport), peerInstanceSequence)') ||
    !closeBody.includes('finalizeActiveDecodedCounter()')) {
  throw new Error('closing a peer must fold its final decoded high-water mark into the run total');
}
for (const required of [
  'const wasCurrent = state.peer === peer',
  'if (state.peer !== peer)',
  'state.peerGeneration = 0',
]) {
  if (!closeBody.includes(required)) {
    throw new Error(`peer close stale-owner guard is missing: ${required}`);
  }
}
if (closeBody.indexOf('if (state.peer !== peer)') >
    closeBody.indexOf('state.activeTrack = null')) {
  throw new Error('a stale close may clear only its captured peer, not a newer track');
}
const acceptStart = html.indexOf(
  'async function acceptOffer(message, epoch, offerSession=state.session)');
const acceptEnd = html.indexOf('\n  async function handleEvent(', acceptStart);
const acceptBody = html.slice(acceptStart, acceptEnd);
for (const required of [
  'const session = offerSession',
  'epoch === state.runEpoch',
  'state.session === session',
  'state.peer === peer',
  'await peer.setRemoteDescription',
  'await peer.createAnswer()',
  'await peer.setLocalDescription(answer)',
  "await postSignal('answer', {sdp:filteredAnswerSdp}, generation, session)",
  'if (await stopStalePeer(peer)) return',
]) {
  if (!acceptBody.includes(required)) {
    throw new Error(`offer async lifetime guard is missing: ${required}`);
  }
}
if (acceptBody.indexOf('if (!offerIsCurrent(null)) return') >
    acceptBody.indexOf("await closePeer('new SDP generation')")) {
  throw new Error('a stale offer must be rejected before it can close the current peer');
}
const signalingStart = html.indexOf('async function signalingLoop(epoch)');
const signalingEnd = html.indexOf('\n  function findVideoInbound(', signalingStart);
const signalingBody = html.slice(signalingStart, signalingEnd);
for (const required of [
  'const pollSession = state.session',
  'state.session !== pollSession',
  'await handleEvent(message, epoch, pollSession)',
  'const oldGeneration = state.generation',
  "postSignal('bye', {}, oldGeneration, oldSession)",
]) {
  if (!signalingBody.includes(required)) {
    throw new Error(`signaling poll lifetime guard is missing: ${required}`);
  }
}
const stopStart = html.indexOf('async function stop(sendBye=true)');
const stopEnd = html.indexOf('\n  async function captureScreenshot()', stopStart);
const stopBody = html.slice(stopStart, stopEnd);
if (!stopBody.includes('const generation = state.generation') ||
    !stopBody.includes("postSignal('bye', {}, generation, session)")) {
  throw new Error('session BYE must use the latest device generation and captured session');
}
if ((html.match(/state\.decodedCompleted = 0/g) || []).length !== 1 ||
    !html.includes('decoded:Math.trunc(decoded)')) {
  throw new Error('decoded counter may reset only at a new run, never at a new track');
}
const trackStart = html.indexOf('peer.ontrack = event =>');
const trackEnd = html.indexOf('\n    peer.onconnectionstatechange', trackStart);
const trackBody = html.slice(trackStart, trackEnd);
if (trackStart < 0 || trackEnd < 0 ||
    trackBody.indexOf('beginIdentityTrack()') >
      trackBody.indexOf('attachFrameCallbacks(epoch, identityTrackToken)')) {
  throw new Error('a new track must establish its identity baseline before rVFC starts');
}
for (const required of [
  'sequence:++state.trackEventSequence', 'generation, peerInstanceSequence',
  'trackId:event.track.id', 'state.trackEvents.push(trackEvent)',
]) {
  if (!trackBody.includes(required)) {
    throw new Error(`structured track event is missing ${required}`);
  }
}
console.log('acceptance page syntax/contracts: OK');
