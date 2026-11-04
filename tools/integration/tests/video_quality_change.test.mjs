import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';
import { decodes, hdmiCapture, webrtcStats } from '../lib/video.mjs';

// Must not change with the quality.
const SAME = ['generation', 'videoEpoch', 'peerRebuilds', 'peerConnects', 'width', 'height', 'fps'];
const SETTLE_MS = 3000;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// A user-set factor is a fixed rate (target = ceiling), applied before the
// encoder's next frame without rebuilding capture or peer.
test('a change of the stream quality reaches the encoder without a rebuild of the capture or a new peer', { timeout: 120_000 }, async t => {
  const mini = await connect();
  // close() stores the stream quality as found.
  t.after(() => mini.close());
  await hdmiCapture(mini);
  const stats = () => webrtcStats(mini);
  const bitrate = async () => {
    const now = await stats();
    return { ceiling: now.videoBitrateCeiling, target: now.videoTargetBitrate };
  };
  const wifi = (await mini.rpc('getDeviceCapabilities')).includes('wifi');
  const stored = factor => until(() => mini.rpc('getStreamQualityFactor'), now => now === factor, 5);

  // The ceiling of factor 1, when a few frames have gone out after the change.
  const sentBefore = (await stats()).accessUnitsSent;
  await mini.rpc('setStreamQualityFactor', { factor: 1 });
  await until(stats, now => now.accessUnitsSent - sentBefore >= 3, 10);
  await decodes(mini);
  const high = await bitrate();
  assert.equal(high.target, high.ceiling, 'factor 1 is a fixed rate');

  for (const factor of [0.5, 1]) {
    const before = await stats();
    const countsBefore = await mini.video();
    await mini.rpc('setStreamQualityFactor', { factor });
    await stored(factor);
    const applied = await until(bitrate, now => (factor < 1 ? now.ceiling < high.ceiling : now.ceiling === high.ceiling), 5)
      .catch(error => { throw new Error(`the encoder did not take factor ${factor} (ceiling ${high.ceiling} at factor 1): ${error.message}`); });
    t.diagnostic(`factor ${factor}: ceiling ${applied.ceiling} bit/s, target ${applied.target} bit/s`);
    assert.equal(applied.target, applied.ceiling, `factor ${factor} is a fixed rate`);
    await decodes(mini);

    // The browser keeps up after the change: in 3 seconds it decodes at
    // least 90 % of the frames that the Mini sends.
    const from = { sent: (await stats()).accessUnitsSent, decoded: (await mini.video()).framesDecoded };
    await sleep(SETTLE_MS);
    const after = await stats();
    const counts = await mini.video();
    const sent = after.accessUnitsSent - from.sent;
    const decoded = counts.framesDecoded - from.decoded;
    assert.ok(sent > 0, 'the Mini sends video');
    assert.ok(decoded >= sent * 0.9, `factor ${factor}: the browser decoded ${decoded} of ${sent} frames`);
    const identity = now => Object.fromEntries(SAME.map(name => [name, now[name]]));
    assert.deepEqual(identity(after), identity(before), `factor ${factor}: the same capture, peer and mode`);
    assert.deepEqual({ modeValid: after.modeValid, sessionActive: after.sessionActive, peerConnected: after.peerConnected },
      { modeValid: true, sessionActive: true, peerConnected: true });
    assert.equal(counts.framesDropped - countsBefore.framesDropped, 0, `factor ${factor}: frames that the browser dropped`);
    // Wi-Fi can freeze briefly regardless; there freezes are only reported.
    const freezes = counts.freezeCount - countsBefore.freezeCount;
    const frozenMs = Math.round((counts.totalFreezesDuration - countsBefore.totalFreezesDuration) * 1000);
    t.diagnostic(`factor ${factor}: ${freezes} freezes of the video, ${frozenMs} ms in all`);
    if (!wifi) assert.equal(freezes, 0, `factor ${factor}: freezes of the video`);
  }
});
