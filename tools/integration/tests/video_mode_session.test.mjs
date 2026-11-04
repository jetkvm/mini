import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hostDisplay } from '../lib/host.mjs';
import { keyboard } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { rpcEvery100ms, watchedSession, whileVideoStopped } from '../lib/session.mjs';
import { decodes, hdmiCapture, webrtcStats } from '../lib/video.mjs';

// A video mode change keeps the peer connection and signaling socket; RPC
// and keys work while video is stopped.

// Modifier keys alone type nothing on the host. The Mini reports the keys
// that it holds down on USB, the modifiers as bits of one byte.
const LEFT_SHIFT = { key: 0xe1, bit: 0x02 };
const RIGHT_CTRL = { key: 0xe4, bit: 0x10 };
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// The Mini's capture mode, asked for every 50 ms: a cleared mode can last
// less than half a second.
async function modeBecomes(mini, valid, seconds) {
  const end = Date.now() + seconds * 1000;
  for (;;) {
    const now = await webrtcStats(mini).catch(() => null);
    if (now?.modeValid === valid) return now;
    if (Date.now() >= end) throw new Error(`the Mini's video mode is not ${valid ? 'valid' : 'cleared'} after ${seconds} s`);
    await sleep(50);
  }
}

// Sends the press or the release of a modifier and waits up to 3 seconds
// until the Mini reports it. The report is asked for every 20 ms, so that
// a press and a release fit into the time for which the video is stopped.
async function setModifier(keys, modifier, down) {
  await keys.press(modifier.key, down);
  await holds(keys, modifier, down);
}
async function holds(keys, modifier, down) {
  const end = Date.now() + 3000;
  while (((await keys.down()).modifier & modifier.bit) !== 0 !== down) {
    if (Date.now() >= end) throw new Error(`the Mini does not report the key 0x${modifier.key.toString(16)} as ${down ? 'held' : 'released'} after 3 s`);
    await sleep(20);
  }
}
async function pressAndRelease(keys, modifier) {
  await setModifier(keys, modifier, true);
  await setModifier(keys, modifier, false);
}

// The page shows video of this size within 30 seconds and decodes frames.
async function showsVideo(mini, session, { width, height }) {
  await until(session.size, size => size?.width === width && size?.height === height, 30)
    .catch(error => { throw new Error(`the page does not show ${width}x${height}: ${error.message}`); });
  await decodes(mini);
}

// A Linux host, whose display settings change the mode.
test('a change of the host\'s resolution keeps the session, and the RPC and the keys work while the video is stopped', { timeout: 240_000 }, async t => {
  const mini = await connect();
  let display = null;
  let traffic = null;
  let keys = null;
  t.after(async () => {
    try {
      await traffic?.stop();
      await keys?.releaseAll().catch(() => undefined);
      await display?.restore();
    } finally {
      await mini.close();
    }
  });
  // First, so that the mode before the test is the one of the Mini's own
  // display and not of a sender's service video.
  await hdmiCapture(mini);
  display = await hostDisplay(t, mini);
  if (!display) return;

  const session = await watchedSession(mini);
  const start = await webrtcStats(mini);
  assert.equal(start.modeValid, true, 'the Mini captures the host\'s HDMI video');
  await showsVideo(mini, session, start);
  const sockets = session.sockets();
  keys = await keyboard(mini);
  // 1080p30 and 720p60 differ in the size of the encoder and of the
  // capture's buffers.
  const other = start.height === 720 ? { width: 1920, height: 1080, hz: 30 } : { width: 1280, height: 720, hz: 60 };

  // One change of the host's mode, with one RPC every 100 ms beside it and
  // a key that is held across it: a Mini that keeps its session keeps the
  // key down. Returns the mode that the Mini captures after it.
  const across = async mode => {
    traffic = rpcEvery100ms(mini);
    await setModifier(keys, LEFT_SHIFT, true);
    // The stop is ~0.5 s, too short to poll, hence whileVideoStopped. The
    // video epoch advances twice per mode change.
    const before = await webrtcStats(mini);
    const stop = await whileVideoStopped(mini, RIGHT_CTRL);
    const had = await display.set(mode);
    const times = await stop.done();
    if (times.stoppedAt === null) {
      const [host, now] = [await display.set(), await webrtcStats(mini)];
      assert.fail(`the Mini told the page of no stop of the video within 15 s. The host had ${had.width}x${had.height}@${had.hz} and has ` +
        `${host.width}x${host.height}@${host.hz}; the Mini's video epoch was ${before.videoEpoch} and is ${now.videoEpoch}, ` +
        `its mode ${now.modeValid ? `${now.width}x${now.height}@${now.fps}` : 'cleared'}`);
    }
    assert.notEqual(times.backAt, null, 'the Mini told the page within 45 s that the video is back');
    const stopped = times.backAt - times.stoppedAt;
    t.diagnostic(`to ${mode.width}x${mode.height}: video stopped for ${stopped} ms; the RPC was answered after ${times.rpcAt - times.stoppedAt} ms, ` +
      `the key was down after ${times.pressedAt - times.stoppedAt} ms and up after ${times.releasedAt - times.stoppedAt} ms`);
    assert.equal(times.rpcError, null, 'the RPC while the video was stopped');
    for (const [name, at] of [['the answer to the RPC', times.rpcAt], ['the report of the pressed key', times.pressedAt],
      ['the report of the released key', times.releasedAt]]) {
      assert.ok(at !== null && at <= times.backAt, `${name} came while the video was stopped (${stopped} ms)`);
    }
    const after = await until(() => webrtcStats(mini), now => now.modeValid && now.videoEpoch >= before.videoEpoch + 2, 45)
      .catch(error => { throw new Error(`the Mini did not build its capture again for the new mode: ${error.message}`); });
    await pressAndRelease(keys, RIGHT_CTRL);
    await showsVideo(mini, session, after);
    await holds(keys, LEFT_SHIFT, true);
    await setModifier(keys, LEFT_SHIFT, false);
    const { sent, failures } = await traffic.stop();
    traffic = null;
    assert.deepEqual(failures, [], `RPC calls that failed, of ${sent}`);
    return after;
  };

  const changed = await across(other);
  assert.equal(`${changed.width}x${changed.height}`, `${other.width}x${other.height}`, 'the Mini captures the new size');
  const back = await across(display.found);
  assert.equal(`${back.width}x${back.height}`, `${start.width}x${start.height}`, 'the Mini captures the size before');

  const peer = await session.peer();
  assert.equal(peer.same, true, 'the page has the same RTCPeerConnection');
  assert.deepEqual(peer.states, [], 'the peer connection did not change its state');
  assert.equal(session.sockets(), sockets, 'the page made no new signaling socket');
  // The Mini's videoInputState events to the page: the video stopped and
  // came back.
  const shown = await mini.events('videoInputState');
  assert.ok(shown.some(state => !state.ready), 'the Mini told the page that the video stopped');
  assert.equal(shown.at(-1).ready, true, 'the Mini told the page that the video is back');
});

// With "Hide display when idle" the Mini takes its display from the host
// while it has no session. A new session gives the display back, and the
// host sets its mode before or after the page has connected. Either way the
// page keeps the one session that it opened.
test('the first session after the Mini hid its display keeps its session when the display returns', { timeout: 180_000 }, async t => {
  const mini = await connect();
  let traffic = null;
  let keys = null;
  t.after(async () => {
    try {
      await traffic?.stop();
      await keys?.releaseAll().catch(() => undefined);
    } finally {
      // close() stores "Hide display when idle" as found.
      await mini.close();
    }
  });
  await hdmiCapture(mini);
  await mini.rpc('setHostDisplayIdleMode', { enabled: true });
  await mini.leave();
  await until(() => webrtcStats(mini), stats => !stats.sessionActive && !stats.modeValid, 30)
    .catch(error => { throw new Error(`the Mini did not hide its display without a session: ${error.message}`); });

  const session = await watchedSession(mini);
  keys = await keyboard(mini);
  traffic = rpcEvery100ms(mini);
  const after = await modeBecomes(mini, true, 60);
  await pressAndRelease(keys, RIGHT_CTRL);
  await showsVideo(mini, session, after);
  const { sent, failures } = await traffic.stop();
  traffic = null;
  assert.deepEqual(failures, [], `RPC calls that failed, of ${sent}`);

  const peer = await session.peer();
  assert.equal(peer.same, true, 'the page has the same RTCPeerConnection');
  assert.deepEqual(peer.states, [], 'the peer connection did not change its state');
  assert.equal(session.sockets(), 1, 'the page made one signaling socket');
});
