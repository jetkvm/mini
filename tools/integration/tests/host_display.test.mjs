import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hostAgent } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hdmiCapture } from '../lib/video.mjs';

// The USB usage of the space key, and its Linux key code on the host.
const HID_SPACE = 0x2c;
const KEY_SPACE = 57;
const INPUT_DEVICES = ['keyboard', 'absolute_mouse', 'relative_mouse'];
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// Presses space on the Mini until the host reports the key; [] when it
// never does. A key also turns on a screen that the host had turned off.
async function keyboardWorks(host, mini, seconds) {
  const end = Date.now() + seconds * 1000;
  while (Date.now() < end) {
    await host.clear('/events/keyboard');
    await mini.rpc('keypressReport', { key: HID_SPACE, press: true });
    await sleep(20);
    await mini.rpc('keypressReport', { key: HID_SPACE, press: false });
    const presses = await until(
      async () => (await host.get('/events/keyboard')).filter(event => event.code === KEY_SPACE && event.type === 'key_press'),
      events => events.length > 0, 3).catch(() => []);
    if (presses.length > 0) return presses;
  }
  return [];
}

// Linux host. Service video would ignore the EDID, so hdmiCapture turns it
// off; a host with its screen off reports a mode only after a key.
test('the host takes the resolution of another EDID preset, and the keyboard works after the change', { timeout: 240_000 }, async t => {
  const host = await hostAgent(t);
  if (!host) return;
  const mini = await connect();
  // The EDID as found, from the moment at which the test may change it.
  let original = null;
  t.after(async () => {
    try {
      if (original === null) return;
      await mini.open();
      if (String(await mini.rpc('getEDID')).toLowerCase() !== original.toLowerCase()) {
        await mini.rpc('setEDID', { edid: original }, 30_000);
      }
    } finally {
      await mini.close();
    }
  });

  const resolution = async () => (await host.get('/display')).find(display => display.status === 'connected')?.resolution ?? null;

  // The host has the Mini's input devices and gets its keys before the
  // change.
  await host.inputDevices(INPUT_DEVICES, 30);
  assert.ok((await keyboardWorks(host, mini, 15)).length > 0, 'the host gets a key before the change');
  await hdmiCapture(mini);

  const startedAt = Date.now();
  const connected = (await host.get('/display')).filter(display => display.status === 'connected');
  assert.ok(connected.length >= 1, 'the host has a connected display');
  assert.ok(connected[0].modes?.length > 0, 'the display has modes');
  assert.match(String(await resolution()), /^\d+x\d+$/);

  const current = await mini.rpc('getEDID');
  const presets = await mini.rpc('getEDIDPresets');
  const target = presets.find(preset =>
    preset.edid.toLowerCase() !== current.toLowerCase() && /(?:1280x720|1920x1080)/.test(preset.name));
  assert.ok(target, 'another preset for 1280x720 or 1920x1080');
  const expected = target.name.match(/(?:1280x720|1920x1080)/)[0];

  original = current;
  try {
    // The Mini's session can end with the change, and its answer with it.
    await mini.rpc('setEDID', { edid: target.edid }, 30_000).catch(() => undefined);
    await sleep(5000);
    await mini.open();
    // The agent may not answer while the host sets its display up again.
    const now = await until(resolution, found => found === expected, 15).catch(() => resolution());
    t.diagnostic(`the preset ${target.name}: the host's resolution is ${now}`);
    assert.equal(now, expected, `the host's resolution with the preset ${target.name}`);
  } finally {
    await mini.rpc('setEDID', { edid: current }, 30_000).catch(() => undefined);
    await sleep(5000);
    await mini.open();
    await host.inputDevices(INPUT_DEVICES, 15);
  }
  assert.ok((await keyboardWorks(host, mini, 30)).length > 0, 'the host gets a key after the EDID is as before');
  assert.ok(Date.now() - startedAt <= 90_000, 'the test takes 90 seconds at most');
});

// Linux host. Idle hiding removes the display 1.8 s after the last session
// ends. Service video stays without a session, so hdmiCapture turns it off.
test('the host keeps the Mini\'s display without a session while idle hiding is off, and has it only during a session while it is on', { timeout: 180_000 }, async t => {
  const host = await hostAgent(t);
  if (!host) return;
  const mini = await connect();
  t.after(() => mini.close());
  await host.inputDevices(INPUT_DEVICES, 30);
  assert.ok((await keyboardWorks(host, mini, 15)).length > 0, 'the host gets a key');
  await hdmiCapture(mini);
  const connected = async () => (await host.get('/display')).filter(display => display.status === 'connected');
  const connectors = displays => displays.map(display => display.connector).sort();
  const missing = (wanted, displays) => wanted.filter(connector => !connectors(displays).includes(connector));
  const startedAt = Date.now();
  const found = await mini.rpc('getHostDisplayIdleMode');

  await mini.rpc('setHostDisplayIdleMode', { enabled: false });
  const kept = await until(connected, displays => displays.length > 0, 15);
  await mini.leave();
  await sleep(3000);
  assert.deepEqual(missing(connectors(kept), await connected()), [], 'with idle hiding off, every display is still connected 3 seconds after the session');

  let hidden = [];
  try {
    await mini.open();
    await mini.rpc('setHostDisplayIdleMode', { enabled: true });
    const shown = connectors(await until(connected, displays => displays.length > 0, 15));
    const leftAt = Date.now();
    await mini.leave();
    // 20 seconds from the end of the page, as for the display's return.
    const idle = await until(connected, displays => missing(shown, displays).length > 0, Math.max(20 - (Date.now() - leftAt) / 1000, 1))
      .catch(() => connected());
    hidden = missing(shown, idle);
    t.diagnostic(`with idle hiding on the host lost ${hidden.join(', ') || 'no display'} ${Date.now() - leftAt} ms after the session`);
    assert.ok(hidden.length > 0, `with idle hiding on, the host loses a display of ${shown.join(', ')} within 20 seconds; it has ${connectors(idle).join(', ')}`);
  } finally {
    await mini.open();
    await mini.rpc('setHostDisplayIdleMode', { enabled: found.enabled });
  }
  const backAt = Date.now();
  await until(connected, displays => missing(hidden, displays).length === 0, 20);
  t.diagnostic(`the host had ${hidden.join(', ')} again ${Date.now() - backAt} ms after the new session`);
  assert.ok(Date.now() - startedAt <= 80_000, 'the test takes 80 seconds at most');
  // The host is left with the mode that it had on each display.
  const modes = displays => displays.map(display => `${display.connector} ${display.resolution}`).sort();
  assert.deepEqual(modes(await until(connected, displays => modes(displays).join() === modes(kept).join(), 20).catch(() => connected())), modes(kept),
    'the host has the mode of each display that it had before the test');
});
