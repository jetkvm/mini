import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';
import { decodes, hdmiCapture, webrtcStats } from '../lib/video.mjs';

// The sleep delay of the test, in seconds; the Hardware page stores 90.
const SHORT = 2;
// The Mini's counts of what its memory budget refused or could not allocate.
const MEMORY_FAILURES = ['admissionDenials', 'videoRebuildBeginDenials', 'videoRebuildFailures', 'jsonAllocationFailures'];
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// HDMI sleep: without it a closed session leaves the capture running; with
// it the capture stops when the last session has been gone for the sleep
// delay, and the next session starts it again, with a new video epoch.
test('HDMI sleep stops the capture after the last session, and a new session starts it again', { timeout: 300_000 }, async t => {
  const mini = await connect();
  // The stored delay as found, from the moment at which the test may change it.
  let found = null;
  t.after(async () => {
    try {
      if (found === null) return;
      await mini.open();
      await mini.rpc('setVideoSleepMode', { duration: found });
      assert.equal((await mini.rpc('getVideoSleepMode')).duration, found, 'the Mini stores the sleep delay as found');
    } finally {
      // close() stores "Hide display when idle" as found.
      await mini.close();
    }
  });
  await hdmiCapture(mini);
  const stats = () => webrtcStats(mini);
  const memoryFailures = async () => {
    const budget = (await stats()).memoryBudget;
    return Object.fromEntries(MEMORY_FAILURES.map(name => [name, budget[name] ?? 0]));
  };
  // Ends the page's session and waits until the Mini has none, with the
  // capture still running or stopped.
  const endSession = async capturing => {
    await mini.leave();
    return until(stats, now => now.started === true && now.sessionActive === false && now.peerConnected === false &&
      now.modeValid === capturing, 20)
      .catch(error => { throw new Error(`no closed session with the capture ${capturing ? 'running' : 'stopped'}: ${error.message}`); });
  };
  // Opens a session and waits until the page decodes video on a peer that
  // is newer than `generation`.
  const newSession = async generation => {
    await mini.open();
    await until(stats, now => now.modeValid === true && now.sessionActive === true && now.peerConnected === true &&
      now.waitingForSync === false && now.generation > generation, 75);
    await decodes(mini);
  };

  const initial = await mini.rpc('getVideoSleepMode');
  assert.equal(initial.supported, true, 'the Mini reports HDMI sleep');
  const failuresBefore = await memoryFailures();
  // With it, a closed session also takes the display from the host, which
  // clears the mode: only the sleep may stop the capture here.
  await mini.rpc('setHostDisplayIdleMode', { enabled: false });

  // Sleep off, as the Hardware page stores it: -1.
  found = initial.duration;
  await mini.rpc('setVideoSleepMode', { duration: -1 });
  assert.ok((await mini.rpc('getVideoSleepMode')).duration < 0, 'the Mini stores sleep as off');
  const awake = await stats();
  await endSession(true);
  await sleep((SHORT + 1) * 1000);
  assert.equal((await stats()).modeValid, true, `the capture runs ${SHORT + 1} s after the session, without sleep`);
  await newSession(awake.generation);

  await mini.rpc('setVideoSleepMode', { duration: SHORT });
  assert.equal((await mini.rpc('getVideoSleepMode')).duration, SHORT);
  const enabledEpoch = (await stats()).videoEpoch;
  const sleeping = await endSession(false);

  await newSession(sleeping.generation);
  const woken = await mini.rpc('getVideoSleepMode');
  assert.deepEqual({ enabled: woken.enabled, duration: woken.duration }, { enabled: false, duration: SHORT }, 'the capture does not sleep in a session');
  assert.ok((await stats()).videoEpoch > enabledEpoch, 'the wake publishes a new video epoch');

  await mini.rpc('setVideoSleepMode', { duration: -1 });
  assert.ok((await mini.rpc('getVideoSleepMode')).duration < 0, 'the Mini stores sleep as off');
  assert.deepEqual(await memoryFailures(), failuresBefore, 'the memory budget refused nothing and no allocation failed');
});
