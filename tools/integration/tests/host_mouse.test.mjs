import assert from 'node:assert/strict';
import { test } from 'node:test';
import { isDeepStrictEqual } from 'node:util';
import { hostAgent } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';
import { MAX_FREEZE_MS, hdmiCapture, watchVideo } from '../lib/video.mjs';

// Linux input codes of the host's events.
const BTN_LEFT = 0x110;
const BTN_RIGHT = 0x111;
const BTN_SIDE = 0x113;
const BTN_EXTRA = 0x114;
const REL_WHEEL = 0x08;
const REL_HWHEEL = 0x06;
const INPUT_DEVICES = ['keyboard', 'absolute_mouse', 'relative_mouse'];
const CENTRE = 16384;
const EDGE = 32767;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// Setup for a mouse test (Linux host, else skipped and null). done(seconds)
// fails on a video freeze or overrun; events() returns [] on timeout.
async function mouseTest(t) {
  const host = await hostAgent(t);
  if (!host) return null;
  const mini = await connect();
  let restoreUsb = null;
  t.after(async () => {
    try {
      await restoreUsb?.();
    } finally {
      await mini.close();
    }
  });
  restoreUsb = await hostTestUsb(mini);
  await host.inputDevices(INPUT_DEVICES, 30);
  // The watch judges a video that runs.
  await until(mini.frames, count => count > 0, 30);
  const startedAt = Date.now();
  const video = watchVideo(mini);
  t.after(video);
  const read = async (types, code) => (await host.get('/events/mouse'))
    .filter(event => types.includes(event.type) && (code === undefined || event.code === code));
  const events = (types, code, seconds) => until(() => read(types, code), found => found.length > 0, seconds).catch(() => []);
  const done = seconds => {
    const freeze = video();
    t.diagnostic(`longest time without a video frame ${freeze.ms} ms`);
    assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
    assert.ok(Date.now() - startedAt <= seconds * 1000, `the test takes ${seconds} seconds at most`);
  };
  return { host, mini, read, events, done };
}

// A Linux host.
test('the host gets each press and release when the right button is pressed while the left one is held', { timeout: 120_000 }, async t => {
  const host = await hostAgent(t);
  if (!host) return;
  const mini = await connect();
  t.after(() => mini.close());
  await host.inputDevices(INPUT_DEVICES, 30);
  // The watch judges a video that runs.
  await until(mini.frames, count => count > 0, 30);
  const startedAt = Date.now();
  const video = watchVideo(mini);
  t.after(video);
  // Bits of `buttons`: 1 is the left button, 2 the right one.
  const report = buttons => mini.rpc('absMouseReport', { x: CENTRE, y: CENTRE, buttons });

  await report(0);
  await sleep(100);
  const attempts = 20;
  let successes = 0;
  for (let i = 0; i < attempts; i++) {
    await host.clear('/events/mouse');
    await report(1);
    await sleep(100);
    await report(3);
    await sleep(100);
    await report(1);
    await sleep(100);
    await report(0);
    await sleep(150);
    const buttons = (await host.get('/events/mouse')).filter(event => event.type === 'mouse_button');
    const has = (code, value) => buttons.some(event => event.code === code && event.value === value);
    if (has(BTN_LEFT, 1) && has(BTN_RIGHT, 1) && has(BTN_RIGHT, 0) && has(BTN_LEFT, 0)) successes++;
  }
  const freeze = video();
  t.diagnostic(`all four button events in ${successes} of ${attempts} rounds; longest time without a video frame ${freeze.ms} ms`);
  assert.equal(successes, attempts, `the host got all four button events in ${successes} of ${attempts} rounds`);
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
  assert.ok(Date.now() - startedAt <= 60_000, 'the test takes 60 seconds at most');
});

// A Linux host. The change of the Mini's USB devices takes the OS Services
// sender off the USB too, so hdmiCapture turns service video off.
test('the wheel works when the relative mouse is the Mini\'s only mouse', { timeout: 180_000 }, async t => {
  const host = await hostAgent(t);
  if (!host) return;
  const mini = await connect();
  // The USB devices as found, from the moment at which the test may change them.
  let original = null;
  // The host can miss a change of the Mini's USB devices, and more time
  // does not help then: the call is sent again every 15 seconds.
  const setDevices = async (devices, types, seconds) => {
    const end = Date.now() + seconds * 1000;
    for (;;) {
      await mini.rpc('setUsbDevices', { devices }).catch(e => {
        if (!/RPC timeout/.test(e.message)) throw e;
      });
      try {
        return await host.inputDevices(types, Math.max(Math.min(15, (end - Date.now()) / 1000), 1));
      } catch (e) {
        if (Date.now() >= end) throw e;
      }
    }
  };
  t.after(async () => {
    try {
      // Once more when the test did not get as far as its own change back.
      if (original !== null && !isDeepStrictEqual(await mini.rpc('getUsbDevices'), original)) {
        await setDevices(original, INPUT_DEVICES, 45);
      }
    } finally {
      await mini.close();
    }
  });
  await hdmiCapture(mini);
  await host.inputDevices(INPUT_DEVICES, 30);
  const found = await mini.rpc('getUsbDevices');
  // The watch judges a video that runs.
  await until(mini.frames, count => count > 0, 30);
  const startedAt = Date.now();
  const video = watchVideo(mini);
  t.after(video);

  original = found;
  await mini.rpc('setUsbDevices', { devices: { ...found, absolute_mouse: false } });
  await host.inputDevices(['keyboard', 'relative_mouse'], 15);

  // The host opens the new devices some time after it lists them: the wheel
  // is turned again until the host reports it, for 10 seconds at most.
  const wheel = async (params, code) => {
    const end = Date.now() + 10_000;
    while (Date.now() < end) {
      await host.clear('/events/mouse');
      await mini.rpc('wheelReport', params);
      const turns = await until(
        async () => (await host.get('/events/mouse')).filter(event => event.type === 'mouse_move_rel' && event.code === code),
        events => events.length > 0, 2).catch(() => []);
      if (turns.length > 0) return turns;
    }
    return [];
  };
  try {
    const vertical = await wheel({ wheelY: 1, wheelX: 0 }, REL_WHEEL);
    assert.ok(vertical.length > 0, 'the host gets the vertical wheel');
    assert.notEqual(vertical[0].value, 0);
    const horizontal = await wheel({ wheelY: 0, wheelX: 1 }, REL_HWHEEL);
    assert.ok(horizontal.length > 0, 'the host gets the horizontal wheel');
    assert.notEqual(horizontal[0].value, 0);
  } finally {
    await setDevices(found, INPUT_DEVICES, 45);
  }
  const freeze = video();
  t.diagnostic(`longest time without a video frame ${freeze.ms} ms`);
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
  assert.ok(Date.now() - startedAt <= 90_000, 'the test takes 90 seconds at most');
});

// A Linux host. The rate, the delay and the place of moves across the screen
// are judged by tools/combined_load, which sends none to an edge of the
// screen.
test('a move to the centre and to each corner of the screen reaches the host', { timeout: 150_000 }, async t => {
  const rig = await mouseTest(t);
  if (!rig) return;
  const { host, mini, events, done } = rig;
  // Not the relative moves, which a jiggler that is on makes too.
  const MOVES = ['mouse_move_abs'];
  const move = (x, y) => mini.rpc('absMouseReport', { x, y, buttons: 0 });

  // A pointer that is at the centre already makes no event of a move to the
  // centre: the pointer goes away from it first, until the host reports a
  // move, for 20 seconds at most.
  await until(async () => {
    await host.clear('/events/mouse');
    await move(0, 0);
    await move(EDGE, EDGE);
    await move(0, 0);
    return events(MOVES, undefined, 3);
  }, moves => moves.length > 0, 20);

  await host.clear('/events/mouse');
  await move(CENTRE, CENTRE);
  assert.ok((await events(MOVES, undefined, 3)).length > 0, 'the host gets the move to the centre');
  for (const [x, y, corner] of [[0, 0, 'top left'], [EDGE, 0, 'top right'], [EDGE, EDGE, 'bottom right'], [0, EDGE, 'bottom left']]) {
    await host.clear('/events/mouse');
    await move(x, y);
    assert.ok((await events(MOVES, undefined, 3)).length > 0, `the host gets the move to the ${corner} corner`);
  }
  done(90);
});

// A Linux host. Bits of `buttons`: 8 is the back button, 16 the forward one.
test('the back and the forward button of the absolute mouse reach the host', { timeout: 120_000 }, async t => {
  const rig = await mouseTest(t);
  if (!rig) return;
  const { host, mini, events, done } = rig;
  const report = buttons => mini.rpc('absMouseReport', { x: CENTRE, y: CENTRE, buttons });
  for (const [buttons, code, name] of [[0x08, BTN_SIDE, 'back'], [0x10, BTN_EXTRA, 'forward']]) {
    await host.clear('/events/mouse');
    await report(buttons);
    await sleep(50);
    await report(0);
    await sleep(50);
    assert.ok((await events(['mouse_button'], code, 3)).length > 0, `the host gets the ${name} button`);
  }
  done(60);
});

// A Linux host. The Mini has both mice, as a user's Mini has.
test('the host gets a wheel turn on each axis, and on both axes from one report', { timeout: 120_000 }, async t => {
  const rig = await mouseTest(t);
  if (!rig) return;
  const { host, mini, read, events, done } = rig;
  const WHEEL = ['mouse_move_rel'];
  const turn = async (wheelY, wheelX) => {
    await host.clear('/events/mouse');
    await mini.rpc('wheelReport', { wheelY, wheelX });
  };

  await turn(1, 0);
  const vertical = await events(WHEEL, REL_WHEEL, 3);
  assert.ok(vertical.length > 0, 'the host gets the vertical wheel');
  assert.notEqual(vertical[0].value, 0);
  await turn(0, 1);
  const horizontal = await events(WHEEL, REL_HWHEEL, 3);
  assert.ok(horizontal.length > 0, 'the host gets the horizontal wheel');
  assert.notEqual(horizontal[0].value, 0);
  await turn(-1, 1);
  assert.ok((await events(WHEEL, REL_WHEEL, 3)).length > 0, 'the host gets the vertical part of a turn on both axes');
  assert.ok((await read(WHEEL, REL_HWHEEL)).length > 0, 'the host gets the horizontal part of a turn on both axes');
  done(60);
});

// A Linux host. The Mini has two mice, and a wheel turn goes to one of them:
// a turn that both sent would scroll twice.
test('the host gets only one event for a wheel turn while the Mini has both mice', { timeout: 120_000 }, async t => {
  const rig = await mouseTest(t);
  if (!rig) return;
  const { host, mini, read, events, done } = rig;
  const WHEEL = ['mouse_move_rel'];
  const turn = async (wheelY, wheelX) => {
    await host.clear('/events/mouse');
    await mini.rpc('wheelReport', { wheelY, wheelX });
  };
  // Every event of the axis that the host has 500 ms after the first one.
  const counted = async code => {
    await sleep(500);
    const turns = await read(WHEEL, code);
    t.diagnostic(`${turns.length} event(s) of the axis ${code} from ${[...new Set(turns.map(event => event.device))].join(', ') || 'no device'}`);
    return turns.length;
  };

  // The turn is sent again until the host reports it, for 10 seconds at most.
  const end = Date.now() + 10_000;
  while (Date.now() < end) {
    await turn(1, 0);
    if ((await events(WHEEL, REL_WHEEL, 2)).length > 0) break;
  }
  assert.equal(await counted(REL_WHEEL), 1, 'one event for one vertical turn');
  await turn(0, 1);
  assert.ok((await events(WHEEL, REL_HWHEEL, 3)).length > 0, 'the host gets the horizontal wheel');
  assert.equal(await counted(REL_HWHEEL), 1, 'one event for one horizontal turn');
  done(30);
});
