import assert from 'node:assert/strict';
import { after, test } from 'node:test';
import { hostAgent } from '../lib/host.mjs';
import { keyboard } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { MAX_FREEZE_MS, watchVideo } from '../lib/video.mjs';

// The Mini auto-releases a non-modifier key 100 ms after its press or last
// keepalive. Tests compare press/release times on a Linux host and share one
// connection.

// Linux key codes of the host's events.
const KEY = {
  LEFT_CTRL: 29, LEFT_SHIFT: 42, LEFT_ALT: 56, RIGHT_ALT: 100, SPACE: 57, BACKSPACE: 14, RIGHT: 106,
  A: 30, B: 48, C: 46, D: 32, E: 18, F: 33, G: 34, H: 35, I: 23, J: 36,
};

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// Polls read() until true, waiting intervals[i] (last one repeats).
async function poll(read, { message = 'the state was not reached', timeout, intervals }) {
  const end = Date.now() + timeout;
  for (let i = 0; ; i++) {
    if ((await read()) === true) return;
    if (Date.now() >= end) assert.fail(`${message} (within ${timeout} ms)`);
    await sleep(intervals[Math.min(i, intervals.length - 1)]);
  }
}

const of = (events, code, type) => events.filter(ev => ev.code === code && ev.type === type);

// Shared connection, created by the first test. Host events carry the
// kernel's time_ms.
let session;
const start = t => (session ??= (async () => {
  const agent = await hostAgent(t);
  if (!agent) return { skip: true };
  const host = { events: () => agent.get('/events/keyboard'), clear: () => agent.clear('/events/keyboard') };
  const mini = await connect();
  try {
    const keys = await keyboard(mini);
    const video = watchVideo(mini);
    await until(async () => {
      await keys.press(0x2c, true);
      await sleep(20);
      await keys.press(0x2c, false);
      await sleep(200);
      return host.events();
    }, events => of(events, KEY.SPACE, 'key_press').length > 0, 15);
    await sleep(200);
    return { mini, host, keys, video };
  } catch (error) {
    await mini.close().catch(() => undefined);
    throw error;
  }
})());

after(async () => {
  if (!session) return;
  const { mini, video } = await session.catch(() => ({}));
  video?.();
  await mini?.close();
});

// body() returns a diagnostic string. Fails on body error, body slower than
// limitMs, or a video freeze.
const keyTest = (name, limitMs, body) => test(`key hold: ${name}`, { timeout: limitMs + 180_000 }, async t => {
  const { skip, video, ...bench } = await start(t);
  // hostAgent() has skipped the first test with the reason.
  if (skip) return t.skip('needs a Linux host with the remote agent');
  const from = Date.now();
  let failure;
  try {
    t.diagnostic(await body(bench));
    if (Date.now() - from > limitMs) assert.fail(`the test took ${Date.now() - from} ms, longer than ${limitMs} ms`);
  } catch (error) {
    failure = error;
  }
  await bench.keys.releaseAll().catch(() => undefined);
  const freeze = video.between(from, Date.now());
  t.diagnostic(`longest time without a video frame ${freeze.ms} ms`);
  if (failure) throw failure;
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
});

keyTest('a modifier that is held alone for 10 s is not released by the Mini', 60_000, async ({ host, keys }) => {
  const modifiers = [
    { hid: 0xe0, linux: KEY.LEFT_CTRL, label: 'LeftCtrl', maskBit: 0x01 },
    { hid: 0xe1, linux: KEY.LEFT_SHIFT, label: 'LeftShift', maskBit: 0x02 },
    { hid: 0xe2, linux: KEY.LEFT_ALT, label: 'LeftAlt', maskBit: 0x04 },
  ];
  const measured = [];
  for (const { hid, linux, label, maskBit } of modifiers) {
    await host.clear();

    await keys.press(hid, true);

    const SAMPLES = 20;
    const SAMPLE_INTERVAL = 500;
    for (let i = 0; i < SAMPLES; i++) {
      await sleep(SAMPLE_INTERVAL);

      const state = await keys.down();
      assert.equal(state?.modifier ?? 0, maskBit,
        `${label} bit should be set on sample ${i + 1}/${SAMPLES} (t=${(i + 1) * SAMPLE_INTERVAL}ms)`);

      const releases = of(await host.events(), linux, 'key_release');
      assert.equal(releases.length, 0, `${label} must not auto-release (sample ${i + 1}/${SAMPLES})`);
    }

    const releaseStart = Date.now();
    await keys.press(hid, false);
    await sleep(200);

    const finalEvents = await host.events();
    const presses = of(finalEvents, linux, 'key_press');
    const releases = of(finalEvents, linux, 'key_release');

    assert.equal(presses.length, 1, `${label} should have exactly 1 press`);
    assert.equal(releases.length, 1, `${label} should have exactly 1 release`);

    const releaseLatency = releases[0].time_ms - presses[0].time_ms;
    assert.ok(releaseLatency > SAMPLES * SAMPLE_INTERVAL - 1000,
      `${label} release should occur after the full 10s hold, and came after ${releaseLatency} ms`);
    const releaseMs = Date.now() - releaseStart;
    assert.ok(releaseMs < 2000, `the release of ${label} and its read took ${releaseMs} ms`);
    measured.push(`${label} held ${releaseLatency} ms on the host`);
  }
  return measured.join(', ');
});

keyTest('a key that is held for 300 ms with keepalives is not released early', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0x04, true);
  await sleep(300);
  await keys.press(0x04, false);
  await sleep(150);

  const events = await host.events();
  const presses = of(events, KEY.A, 'key_press');
  const releases = of(events, KEY.A, 'key_release');

  assert.ok(presses.length >= 1, 'Key A should have been pressed');
  assert.equal(releases.length, 1, 'Key A should have exactly one release');

  const holdDuration = releases[0].time_ms - presses[0].time_ms;
  assert.ok(holdDuration >= 250, `Key should be held for at least 250ms (keepalives kept it alive), and was for ${holdDuration} ms`);
  return `A held ${holdDuration} ms on the host`;
});

keyTest('a key without keepalives is released by the Mini near 100 ms', 60_000, async ({ mini, host }) => {
  // The JSON-RPC keypressReport sends the key alone, without a keepalive.
  await host.clear();

  await mini.rpc('keypressReport', { key: 0x04, press: true });
  await sleep(300);

  const events = await host.events();
  const presses = of(events, KEY.A, 'key_press');
  const releases = of(events, KEY.A, 'key_release');

  assert.ok(presses.length >= 1, 'Key A should have been pressed');
  assert.ok(releases.length >= 1, 'Key A should have auto-released');

  const holdDuration = releases[0].time_ms - presses[0].time_ms;
  assert.ok(holdDuration < 200, `Auto-release should fire near 100ms, and came after ${holdDuration} ms`);
  return `A released after ${holdDuration} ms on the host`;
});

keyTest('an arrow key that is held for 500 ms is not released early', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0x4f, true); // Right Arrow
  await sleep(500);
  await keys.press(0x4f, false);
  await sleep(150);

  const events = await host.events();
  const presses = of(events, KEY.RIGHT, 'key_press');
  const releases = of(events, KEY.RIGHT, 'key_release');

  assert.ok(presses.length >= 1, 'Right arrow should have been pressed');
  assert.equal(releases.length, 1, 'Right arrow should have exactly one release');

  const holdDuration = releases[0].time_ms - presses[0].time_ms;
  assert.ok(holdDuration >= 400, `Right arrow should be held for at least 400ms, and was for ${holdDuration} ms`);
  return `Right Arrow held ${holdDuration} ms on the host`;
});

keyTest('Shift stays held while ten keys are tapped over 10 s', 30_000, async ({ host, keys }) => {
  await host.clear();

  // A to J
  const letters = [0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d];

  await keys.press(0xe1, true); // Shift down
  await sleep(500);

  for (const hid of letters) {
    await keys.press(hid, true);
    await sleep(50);
    await keys.press(hid, false);
    await sleep(950); // ~1s between keys
  }

  await keys.press(0xe1, false); // Shift up
  await sleep(300);

  const events = await host.events();

  const shiftPresses = of(events, KEY.LEFT_SHIFT, 'key_press');
  const shiftReleases = of(events, KEY.LEFT_SHIFT, 'key_release');
  assert.equal(shiftPresses.length, 1, 'Shift should have exactly 1 press');
  assert.equal(shiftReleases.length, 1, 'Shift should have exactly 1 release (no premature auto-release)');

  const expectedLinux = [KEY.A, KEY.B, KEY.C, KEY.D, KEY.E, KEY.F, KEY.G, KEY.H, KEY.I, KEY.J];
  for (const code of expectedLinux) {
    assert.ok(of(events, code, 'key_press').length >= 1, `Key ${code} should have been pressed`);
  }

  const lastLetterPress = Math.max(
    ...events.filter(ev => expectedLinux.includes(ev.code) && ev.type === 'key_press').map(ev => ev.time_ms),
  );
  assert.ok(shiftReleases[0].time_ms > lastLetterPress, 'Shift release must come after all letter key presses');
  return `Shift held ${shiftReleases[0].time_ms - shiftPresses[0].time_ms} ms on the host, ` +
    `released ${shiftReleases[0].time_ms - lastLetterPress} ms after the last letter's press`;
});

keyTest('a key that is held for 2 s with keepalives stays held', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0x2a, true); // Backspace
  await sleep(2000);
  await keys.press(0x2a, false);
  await sleep(150);

  const events = await host.events();
  const presses = of(events, KEY.BACKSPACE, 'key_press');
  const releases = of(events, KEY.BACKSPACE, 'key_release');

  assert.ok(presses.length >= 1, 'Backspace should have been pressed');
  assert.equal(releases.length, 1, 'Backspace should have exactly one release');

  const holdDuration = releases[0].time_ms - presses[0].time_ms;
  assert.ok(holdDuration >= 1800, `Backspace should be held for at least 1800ms, and was for ${holdDuration} ms`);
  return `Backspace held ${holdDuration} ms on the host`;
});

keyTest('a modifier survives a gap in the keepalives in the middle of a chord', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0xe1, true); // Shift down
  await sleep(80);

  // Longer than the 100 ms auto-release.
  await keys.pause(250);

  await sleep(50);
  await keys.press(0x04, true); // A down
  await poll(async () => of(await host.events(), KEY.A, 'key_press').length > 0, { timeout: 2000, intervals: [20] });
  await keys.press(0x04, false); // A up

  await sleep(250);

  const events = await host.events();
  const shiftPressesPreRelease = of(events, KEY.LEFT_SHIFT, 'key_press');
  const shiftReleasesPreRelease = of(events, KEY.LEFT_SHIFT, 'key_release');
  assert.equal(shiftPressesPreRelease.length, 1, 'Shift pressed exactly once');
  assert.equal(shiftReleasesPreRelease.length, 0, 'Shift must NOT have been auto-released during the keepalive gap');

  const aPresses = of(events, KEY.A, 'key_press');
  assert.ok(aPresses.length >= 1, 'A should have been pressed inside the gap');
  assert.ok(aPresses[0].time_ms > shiftPressesPreRelease[0].time_ms, 'A should have been pressed after Shift');

  await keys.press(0xe1, false);
  await sleep(200);

  const finalEvents = await host.events();
  const shiftReleases = of(finalEvents, KEY.LEFT_SHIFT, 'key_release');
  assert.equal(shiftReleases.length, 1, 'Shift should have exactly 1 release (the explicit one)');
  return `A pressed ${aPresses[0].time_ms - shiftPressesPreRelease[0].time_ms} ms after Shift, ` +
    `Shift held ${shiftReleases[0].time_ms - shiftPressesPreRelease[0].time_ms} ms on the host`;
});

keyTest('a key that the Mini released under a modifier does not shorten the next key of the chord', 15_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0xe1, true); // Shift down
  await sleep(80);
  await keys.press(0x04, true); // A down
  await sleep(80);
  await keys.pause(5000);
  await sleep(300);

  await poll(async () => {
    const state = await keys.down();
    return state?.modifier === 0x02 && state.keys.every(k => k === 0);
  }, { message: 'A should auto-release while LeftShift remains held', timeout: 5000, intervals: [100, 200, 500] });

  await sleep(3000);

  await host.clear();
  await keys.press(0x05, true); // B down
  // Measure the hold from the Mini's report, not the send.
  await poll(async () => {
    const state = await keys.down();
    return state?.modifier === 0x02 && state.keys.includes(0x05);
  }, { timeout: 2000, intervals: [20] });
  const bPressStart = Date.now();

  for (const t of [50, 150, 300, 450]) {
    await sleep(Math.max(0, t - (Date.now() - bPressStart)));
    const state = await keys.down();
    assert.equal(state?.modifier === 0x02 && state.keys.includes(0x05), true, `B should still be held with LeftShift at t+${t}ms`);
  }

  await keys.press(0x05, false);
  await keys.press(0x04, false);
  await keys.press(0xe1, false);
  await sleep(200);

  const events = await host.events();
  const bPresses = of(events, KEY.B, 'key_press');
  const bReleases = of(events, KEY.B, 'key_release');
  const shiftReleases = of(events, KEY.LEFT_SHIFT, 'key_release');

  assert.equal(bPresses.length, 1, 'B pressed exactly once');
  assert.equal(bReleases.length, 1, 'B should have exactly one release');
  assert.equal(shiftReleases.length, 1, 'LeftShift should release only explicitly');

  const holdDuration = bReleases[0].time_ms - bPresses[0].time_ms;
  assert.ok(holdDuration >= 400, `B should not auto-release at ~100ms under LeftShift, and was held for ${holdDuration} ms`);
  return `B held ${holdDuration} ms on the host`;
});

keyTest('a hold of a modifier alone does not shorten the next hold', 15_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0xe1, true);
  await sleep(1000);
  await keys.press(0xe1, false);

  await sleep(3000);

  await host.clear();
  await keys.press(0x04, true);
  await poll(async () => (await keys.down())?.keys.includes(0x04), { timeout: 2000, intervals: [20] });
  const aPressStart = Date.now();

  for (const t of [50, 150, 300, 450]) {
    await sleep(t - (Date.now() - aPressStart));
    const state = await keys.down();
    assert.equal(state?.keys?.includes(0x04) ?? false, true, `'a' should still be held in keysDownState at t+${t}ms (cross-hold reset working)`);
  }

  await keys.press(0x04, false);
  await sleep(200);

  const events = await host.events();
  const aPresses = of(events, KEY.A, 'key_press');
  const aReleases = of(events, KEY.A, 'key_release');
  assert.equal(aPresses.length, 1, 'A pressed exactly once');
  assert.equal(aReleases.length, 1, 'A should have exactly one release (no premature auto-release)');

  const holdDuration = aReleases[0].time_ms - aPresses[0].time_ms;
  assert.ok(holdDuration >= 400, `A should be held for ~500ms, and was for ${holdDuration} ms`);
  return `A held ${holdDuration} ms on the host`;
});

keyTest('a key that the Mini released does not shorten the next hold', 15_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0x04, true);
  await sleep(80);
  await keys.pause(5000);
  await sleep(300);

  await poll(async () => {
    const state = await keys.down();
    return state?.modifier === 0 && state.keys.every(k => k === 0);
  }, { message: 'A should auto-release to an empty device state', timeout: 5000, intervals: [100, 200, 500] });

  await sleep(3000);

  await host.clear();
  const bPressStart = Date.now();
  await keys.press(0x05, true);

  for (const t of [50, 150, 300, 450]) {
    await sleep(Math.max(0, t - (Date.now() - bPressStart)));
    const state = await keys.down();
    assert.equal(state?.keys?.includes(0x05) ?? false, true, `B should still be held in keysDownState at t+${t}ms after auto-release reset`);
  }

  await keys.press(0x05, false);
  await keys.press(0x04, false);
  await sleep(200);

  const events = await host.events();
  const bPresses = of(events, KEY.B, 'key_press');
  const bReleases = of(events, KEY.B, 'key_release');
  assert.equal(bPresses.length, 1, 'B pressed exactly once');
  assert.equal(bReleases.length, 1, 'B should have exactly one release');

  const holdDuration = bReleases[0].time_ms - bPresses[0].time_ms;
  assert.ok(holdDuration >= 400, `B should be held for ~500ms, not auto-release at ~100ms, and was for ${holdDuration} ms`);
  return `B held ${holdDuration} ms on the host`;
});

keyTest('a modifier that is pressed without keepalives is not released by the Mini', 60_000, async ({ mini, host }) => {
  await host.clear();
  const heldAlone = async () => {
    const state = await mini.rpc('getKeyDownState');
    return state.modifier === 0x02 && state.keys.every(k => k === 0);
  };
  const intervals = [100, 200, 500];
  try {
    // The JSON-RPC keypressReport sends the key alone, without a keepalive.
    const pressStart = Date.now();
    await mini.rpc('keypressReport', { key: 0xe1, press: true });

    await poll(heldAlone, { message: 'LeftShift should be held after direct keypressReport', timeout: 5000, intervals });
    await poll(async () => of(await host.events(), KEY.LEFT_SHIFT, 'key_press').length > 0,
      { message: 'Host should see LeftShift press', timeout: 5000, intervals });

    await sleep(300);

    const state = await mini.rpc('getKeyDownState');
    assert.equal(state.modifier, 0x02, 'LeftShift should still be held without keepalives');
    const releases = of(await host.events(), KEY.LEFT_SHIFT, 'key_release');
    assert.equal(releases.length, 0, 'LeftShift must not auto-release without keepalives');
    return `LeftShift held ${Date.now() - pressStart} ms without a keepalive, 0 releases on the host`;
  } finally {
    await mini.rpc('keypressReport', { key: 0xe1, press: false }).catch(() => undefined);
    await poll(async () => of(await host.events(), KEY.LEFT_SHIFT, 'key_release').length > 0, { timeout: 2000, intervals: [20] })
      .catch(() => undefined);
  }
});

keyTest('Shift is released once, after A, when A is held and released under it', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0xe1, true); // Shift down
  await sleep(50);
  await keys.press(0x04, true); // A down
  await sleep(200);
  await keys.press(0x04, false);
  await sleep(50);
  await keys.press(0xe1, false);
  await sleep(150);

  const events = await host.events();
  const shiftReleases = of(events, KEY.LEFT_SHIFT, 'key_release');
  const aReleases = of(events, KEY.A, 'key_release');

  assert.equal(shiftReleases.length, 1, 'Shift should have exactly one release');
  assert.equal(aReleases.length, 1, 'A should have exactly one release');
  assert.ok(shiftReleases[0].time_ms > aReleases[0].time_ms, 'Shift must be released after A');
  return `Shift released ${shiftReleases[0].time_ms - aReleases[0].time_ms} ms after A on the host`;
});

keyTest('Shift is released once when the press of A repeats 20 times under it', 60_000, async ({ host, keys }) => {
  // Browser auto-repeat: repeated presses without releases.
  await host.clear();

  await keys.press(0xe1, true); // Shift down
  await sleep(50);

  await keys.sequence(Array.from({ length: 20 }, () => [0x04, true, 30]));

  await keys.press(0x04, false);
  await sleep(200);
  await keys.press(0xe1, false);
  await sleep(200);

  const events = await host.events();
  const shiftReleases = of(events, KEY.LEFT_SHIFT, 'key_release');
  assert.equal(shiftReleases.length, 1, 'Shift should have exactly 1 release');
  return `Shift held ${shiftReleases[0].time_ms - of(events, KEY.LEFT_SHIFT, 'key_press')[0]?.time_ms} ms on the host`;
});

keyTest('Shift stays held while 20 keys are tapped 50 ms apart, and the host gets every key once', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0xe1, true);
  await sleep(50);

  // A to J, twice: each held for 20 ms, 30 ms before the next.
  const taps = Array.from({ length: 20 }, (_, i) => i % 10);
  await keys.sequence(taps.flatMap(i => [[0x04 + i, true, 20], [0x04 + i, false, 30]]));

  await keys.press(0xe1, false);

  // Wait until the host has been quiet for 500 ms after Shift's release, so a
  // duplicate release would show. Sending a key here would hide a lost report.
  let events = [];
  let previousSnapshot = '';
  let quietSince = performance.now();
  await poll(async () => {
    events = await host.events();
    const snapshot = JSON.stringify(events);
    if (snapshot !== previousSnapshot) {
      previousSnapshot = snapshot;
      quietSince = performance.now();
    }
    const last = events.at(-1);
    return last?.code === KEY.LEFT_SHIFT && last.type === 'key_release' && performance.now() - quietSince >= 500;
  }, { message: 'host keyboard events should end with the release of Shift and settle', timeout: 5000, intervals: [50] });

  const letters = [KEY.A, KEY.B, KEY.C, KEY.D, KEY.E, KEY.F, KEY.G, KEY.H, KEY.I, KEY.J];
  const expected = [
    { code: KEY.LEFT_SHIFT, type: 'key_press' },
    ...taps.flatMap(i => [{ code: letters[i], type: 'key_press' }, { code: letters[i], type: 'key_release' }]),
    { code: KEY.LEFT_SHIFT, type: 'key_release' },
  ];
  assert.deepEqual(events.map(({ code, type }) => ({ code, type })), expected, 'complete host key sequence');
  return `${events.length} events on the host, Shift held ${events.at(-1).time_ms - events[0].time_ms} ms`;
});

keyTest('Ctrl and Shift are each released once when A is tapped under both', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0xe0, true); // Ctrl
  await keys.press(0xe1, true); // Shift
  await sleep(50);
  await keys.press(0x04, true); // A
  await sleep(50);
  await keys.press(0x04, false);
  await sleep(200);
  await keys.press(0xe1, false);
  await keys.press(0xe0, false);
  await sleep(200);

  const events = await host.events();
  const held = [];
  for (const [code, label] of [[KEY.LEFT_CTRL, 'Ctrl'], [KEY.LEFT_SHIFT, 'Shift']]) {
    const releases = of(events, code, 'key_release');
    assert.equal(releases.length, 1, `${label} should have exactly 1 release`);
    held.push(`${label} held ${releases[0].time_ms - of(events, code, 'key_press')[0]?.time_ms} ms on the host`);
  }
  assert.ok(of(events, KEY.A, 'key_press').length >= 1, 'A should have been pressed');
  return held.join(', ');
});

keyTest('Shift that is released before A is released once, before A', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0xe1, true); // Shift
  await sleep(50);
  await keys.press(0x04, true); // A
  await sleep(100);
  await keys.press(0xe1, false); // Shift up first
  await sleep(100);
  await keys.press(0x04, false); // A up
  await sleep(200);

  const events = await host.events();
  const shiftRelease = of(events, KEY.LEFT_SHIFT, 'key_release');
  const aRelease = of(events, KEY.A, 'key_release');

  assert.equal(shiftRelease.length, 1, 'Shift should have exactly 1 release');
  assert.equal(aRelease.length, 1, 'A should have exactly 1 release');
  assert.ok(shiftRelease[0].time_ms < aRelease[0].time_ms, 'Shift must be released before A');
  return `A released ${aRelease[0].time_ms - shiftRelease[0].time_ms} ms after Shift on the host`;
});

keyTest('Right Alt (AltGr) is released once when A is tapped under it', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0xe6, true); // AltRight
  await sleep(50);
  await keys.press(0x04, true); // A
  await sleep(50);
  await keys.press(0x04, false);
  await sleep(200);
  await keys.press(0xe6, false);
  await sleep(200);

  const events = await host.events();
  const altReleases = of(events, KEY.RIGHT_ALT, 'key_release');
  assert.equal(altReleases.length, 1, 'AltRight should have exactly 1 release');
  return `Right Alt held ${altReleases[0].time_ms - of(events, KEY.RIGHT_ALT, 'key_press')[0]?.time_ms} ms on the host`;
});

keyTest('three keys that are held together for 300 ms are each released once', 60_000, async ({ host, keys }) => {
  await host.clear();

  await keys.press(0x04, true); // A
  await keys.press(0x05, true); // B
  await keys.press(0x06, true); // C
  await sleep(300);
  await keys.press(0x04, false);
  await keys.press(0x05, false);
  await keys.press(0x06, false);
  await sleep(150);

  const events = await host.events();
  const held = [];
  for (const [code, label] of [[KEY.A, 'A'], [KEY.B, 'B'], [KEY.C, 'C']]) {
    const presses = of(events, code, 'key_press');
    const releases = of(events, code, 'key_release');
    assert.ok(presses.length >= 1, `${label} should have at least 1 press`);
    assert.equal(releases.length, 1, `${label} should have exactly 1 release`);
    held.push(`${label} held ${releases[0].time_ms - presses[0].time_ms} ms on the host`);
  }
  return held.join(', ');
});

keyTest('20 taps of a key 30 ms apart give 20 presses and 20 releases on the host', 60_000, async ({ host, keys }) => {
  const TAP_COUNT = 20;
  await host.clear();

  // Each tap holds A for 10 ms, 20 ms before the next.
  await keys.sequence(Array.from({ length: TAP_COUNT }, () => [[0x04, true, 10], [0x04, false, 20]]).flat());

  await sleep(500);

  const events = await host.events();
  const presses = of(events, KEY.A, 'key_press');
  const releases = of(events, KEY.A, 'key_release');

  assert.equal(presses.length, TAP_COUNT, `Should have ${TAP_COUNT} presses`);
  assert.equal(releases.length, TAP_COUNT, `Should have ${TAP_COUNT} releases (no phantom releases)`);
  const holds = releases.map((release, i) => release.time_ms - presses[i].time_ms);
  return `A held ${Math.min(...holds)} to ${Math.max(...holds)} ms on the host, ` +
    `the 20 taps took ${releases.at(-1).time_ms - presses[0].time_ms} ms`;
});
