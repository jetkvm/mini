import assert from 'node:assert/strict';
import { test } from 'node:test';
import { setTimeout as sleep } from 'node:timers/promises';
import { isDeepStrictEqual } from 'node:util';
import { hostAgent } from '../lib/host.mjs';
import { keyboard, spaceReachesHost } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';
import { MAX_FREEZE_MS, hdmiCapture, watchVideo } from '../lib/video.mjs';

const INPUT_DEVICES = ['keyboard', 'absolute_mouse', 'relative_mouse'];
// The USB usage of the A key, and the Linux codes of that key and of the
// left mouse button on the host.
const HID_A = 0x04;
const KEY_A = 30;
const BTN_LEFT = 0x110;

// What /webrtc/stats says about the owner of the keyboard and the mice: the
// Mini numbers its browser sessions (generation), and takes input only from
// the session whose number is the active one, and only while its USB is on.
// With `session`, also what the Mini answers that session about its USB.
async function observe(mini, session) {
  const response = await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(3000) });
  assert.ok(response.ok, `/webrtc/stats answered ${response.status}`);
  const stats = await response.json();
  return {
    usbService: stats.usbService,
    usbHid: stats.usbHid,
    generation: stats.generation,
    peerConnected: stats.peerConnected,
    peerConnects: stats.peerConnects,
    channels: stats.jetkvmDataChannels,
    ...(session ? { getter: await session.rpc('getUsbEmulationState') } : {}),
  };
}

// The USB service has finished a change to on or off, without an error.
const settled = (service, enabled) => service.initialized && service.desiredEnabled === enabled && service.driverEnabled === enabled &&
  service.hidInputReady === enabled && !service.transitioning && !service.commandPending && service.lastError === 0;

// The session that was asked has the input with `generation`, and the Mini
// takes its input exactly when its USB is on.
const owns = (o, enabled, generation) => generation > 0 && o.getter === enabled && settled(o.usbService, enabled) &&
  o.usbHid.initialized && o.usbHid.usbAvailable === enabled && o.usbHid.acceptingInput === enabled &&
  o.usbHid.activeGeneration === generation && o.peerConnected && o.channels.rpcOpen && o.channels.hidrpcOpen &&
  o.channels.hidrpcHandshakeComplete && o.generation === generation && o.channels.hidrpcActiveGeneration === generation;

// No session, with the Mini's USB off: nobody has the input.
const noSession = o => settled(o.usbService, false) && o.usbHid.initialized && !o.usbHid.usbAvailable && !o.usbHid.acceptingInput &&
  o.usbHid.activeGeneration === 0 && !o.peerConnected && !o.channels.rpcOpen && !o.channels.hidrpcOpen &&
  o.channels.hidrpcActiveGeneration === 0;

// The Mini's USB Serial/JTAG debug interface (303a:1001) on the host: a
// second USB device of the Mini, whose serial is the Mini's device ID as a
// MAC address. One line with its sysfs path, bus number and device number.
const debugInterface = serial =>
  'for d in /sys/bus/usb/devices/*; do [ -f "$d/idVendor" ] && [ -f "$d/serial" ] || continue; ' +
  '[ "$(cat "$d/idVendor"):$(cat "$d/idProduct")" = 303a:1001 ] || continue; ' +
  `[ "$(tr a-f A-F < "$d/serial")" = "${serial}" ] || continue; ` +
  'echo "$d $(cat "$d/busnum") $(cat "$d/devnum")"; done';

// Input belongs to the single session; the old session's input must not reach
// the host, and the USB device must stay enumerated throughout. Linux host.
test('a new session takes the keyboard and the mouse, with USB on and off, and the host keeps the USB device without a session', { timeout: 600_000 }, async t => {
  const host = await hostAgent(t);
  if (!host) return;
  const mini = await connect();
  let restoreUsb = null;
  let second = null;
  t.after(async () => {
    try {
      await second?.close().catch(() => undefined);
      await mini.open();
      // A button that a failed test left pressed.
      await mini.rpc('absMouseReport', { x: 17000, y: 17000, buttons: 0 }).catch(() => undefined);
      await restoreUsb?.();
    } finally {
      await mini.close();
    }
  });
  restoreUsb = await hostTestUsb(mini);
  await hdmiCapture(mini);
  await host.inputDevices(INPUT_DEVICES, 30);
  // The watch judges a video that runs.
  await until(mini.frames, count => count > 0, 30);
  const startedAt = Date.now();
  const video = watchVideo(mini);
  t.after(video);

  // The left button's presses (1) and releases (0) that the host got.
  const leftButton = async () => (await host.get('/events/mouse'))
    .filter(event => event.type === 'mouse_button' && event.code === BTN_LEFT).map(event => event.value);
  const hostGetsButton = (value, what) => until(leftButton, values => values.includes(value), 5)
    .catch(e => assert.fail(`${what} within 5 seconds: ${e.message}`));
  const hostGetsKey = async (session, what) => assert.ok((await spaceReachesHost(session, host, 30)).length > 0, `${what} within 30 seconds`);

  // The first session types and moves the mouse. The host opens a mouse
  // some time after it lists it, so the moves are sent again for 20 seconds.
  const first = await keyboard(mini);
  await hostGetsKey(mini, 'the host gets a key of the first session');
  await until(async () => {
    await host.clear('/events/mouse');
    await first.pointer(0, 0, 0);
    await first.pointer(32767, 32767, 0);
    await first.pointer(0, 0, 0);
    return until(() => host.get('/events/mouse'), events => events.some(event => event.type === 'mouse_move_abs'), 3);
  }, () => true, 20).catch(e => assert.fail(`the host gets a mouse move of the first session within 20 seconds: ${e.message}`));
  await host.clear('/events/mouse');
  await first.pointer(16000, 16000, 1);
  await hostGetsButton(1, 'the host gets the button press of the first session');
  const firstGeneration = (await observe(mini)).generation;

  // A second session takes over while the first one holds the button. Its
  // release, on its own HID channel, reaches the host, and so does its key.
  second = await mini.session();
  const other = await keyboard(second);
  await other.pointer(16000, 16000, 0);
  await hostGetsButton(0, 'the host gets the button release of the second session');
  await hostGetsKey(second, 'the host gets a key of the second session');
  const taken = await until(() => observe(mini, second), o => owns(o, true, o.usbHid.activeGeneration), 20);
  assert.notEqual(taken.generation, firstGeneration, 'the second session has a generation of its own');
  // What the first session's page still sends: its channel is closed, or
  // the Mini drops the key.
  await host.clear('/events/keyboard');
  await first.press(HID_A, true).catch(() => undefined);
  await first.press(HID_A, false).catch(() => undefined);
  await sleep(1000);
  assert.deepEqual((await host.get('/events/keyboard')).filter(event => event.code === KEY_A), [],
    'the host gets no key of the first session after the takeover');

  // The first page takes the session back, as its "Use Here" button does:
  // it opens a new session. A press and a release on that session's HID
  // channel reach the host, each once.
  video.sessionEnded();
  await mini.open();
  const again = await keyboard(mini);
  await host.clear('/events/mouse');
  await again.pointer(17000, 17000, 1);
  await hostGetsButton(1, 'the host gets the button press of the session that took over again');
  await again.pointer(17000, 17000, 0);
  await until(leftButton, values => isDeepStrictEqual(values, [1, 0]), 5)
    .catch(e => assert.fail(`the host gets exactly one press and one release within 5 seconds: ${e.message}`));
  await second.close();
  second = null;
  const takeoverEnd = Date.now();

  // With USB off: generations still advance per session, and USB on gives
  // input to the current one. The debug interface must not be reset.
  const deviceId = await mini.rpc('getDeviceID');
  assert.match(deviceId, /^[0-9a-f]{12}$/i);
  const debug = async () => {
    const lines = (await host.exec(debugInterface(deviceId.match(/../g).join(':').toUpperCase()))).trim().split('\n').filter(Boolean);
    assert.ok(lines.length <= 1, `the host has ${lines.length} debug interfaces with the Mini's serial`);
    return lines[0] ?? null;
  };
  const debugBefore = await debug();
  if (debugBefore === null) t.diagnostic('the host has no debug interface (303a:1001) of this Mini: its reset is not checked');
  // The session decodes new frames; returns Chrome's count of its freezes.
  const videoPlays = async () => {
    const from = (await mini.video())?.framesDecoded ?? 0;
    return (await until(mini.video, now => now?.framesDecoded > from, 15)).freezeCount;
  };
  const read = () => observe(mini, mini);
  const reached = (what, check) => until(read, check, 20).catch(e => assert.fail(`${what} within 20 seconds: ${e.message}`));

  const oldEnabled = await reached('the session has the input with USB on', o => owns(o, true, o.usbHid.activeGeneration));
  const oldGeneration = oldEnabled.usbHid.activeGeneration;
  const debugSeen = [await debug()];
  const freezesBefore = await videoPlays();

  assert.equal(await mini.rpc('setUsbEmulationState', { enabled: false }), null);
  const oldDisabled = await reached(`the session keeps generation ${oldGeneration} and the Mini takes no input`, o => owns(o, false, oldGeneration));
  debugSeen.push(await debug());
  assert.equal(await videoPlays(), freezesBefore, 'the video did not freeze while USB was turned off');

  await mini.leave();
  await until(() => observe(mini), noSession, 20).catch(e => assert.fail(`the ended session leaves no owner of the input within 20 seconds: ${e.message}`));
  video.sessionEnded();
  await mini.open();
  const newDisabled = await reached('the next session has a new generation and the Mini takes no input',
    o => o.usbHid.activeGeneration !== oldGeneration && owns(o, false, o.usbHid.activeGeneration));
  const newGeneration = newDisabled.usbHid.activeGeneration;
  debugSeen.push(await debug());
  const freezesAfter = await videoPlays();

  assert.equal(await mini.rpc('setUsbEmulationState', { enabled: true }), null);
  const newEnabled = await reached(`USB on opens the input for generation ${newGeneration}`, o => owns(o, true, newGeneration));
  debugSeen.push(await debug());
  assert.equal(await videoPlays(), freezesAfter, 'the video did not freeze while USB was turned on');
  assert.deepEqual(debugSeen, debugSeen.map(() => debugBefore), 'the host kept the debug interface: the same path, bus and device number');

  // Only the two calls changed the USB service: a stored change of the
  // configuration each (jetkvm_usb_service_set_enabled), and a session is none.
  const services = [oldEnabled, oldDisabled, newDisabled, newEnabled].map(o => o.usbService);
  for (const count of ['configurationRequests', 'configurationTransactionsCompleted']) {
    const counts = services.map(service => service[count]);
    assert.ok(counts[1] >= counts[0] + 1, `${count} rose with USB off: ${counts}`);
    assert.equal(counts[2], counts[1], `${count} did not change with the new session: ${counts}`);
    assert.ok(counts[3] >= counts[2] + 1, `${count} rose with USB on: ${counts}`);
  }
  for (const service of services.slice(1)) {
    for (const count of ['transitionFailures', 'configurationTransactionFailures', 'configurationRollbacks']) {
      assert.equal(service[count], services[0][count], count);
    }
    assert.equal(service.lastError, 0);
  }
  assert.ok(newDisabled.peerConnects >= oldDisabled.peerConnects + 1, 'the next session is a new connection');
  // The host has the Mini again, and the session that came while USB was off types.
  await host.inputDevices(INPUT_DEVICES, 30);
  await hostGetsKey(mini, 'the host gets a key of the new session after USB is on again');
  const usbEnd = Date.now();

  // Without a session the Mini stays on the host's USB: the same bus and
  // device number for 30 seconds, which a new enumeration would change. The
  // session after it types.
  const config = await mini.rpc('getUsbConfig');
  const usbId = `${config.vendor_id.replace(/^0x/, '')}:${config.product_id.replace(/^0x/, '')}`.toLowerCase();
  const onHost = async () => ((await host.get('/usb/devices')) ?? []).filter(device => device.id.toLowerCase() === usbId);
  const before = await onHost();
  assert.equal(before.length, 1, `the host has one USB device ${usbId}`);
  assert.match(before[0].bus, /^[1-9]\d*$/);
  assert.match(before[0].device, /^[1-9]\d*$/);
  await mini.leave();
  for (let sample = 0; sample < 6; sample++) {
    await sleep(5000);
    assert.deepEqual(await onHost(), before, `the host has the same USB device ${(sample + 1) * 5} seconds after the session ended`);
  }
  video.sessionEnded();
  await mini.open();
  await hostGetsKey(mini, 'the host gets a key of the session that came after 30 seconds without one');

  // The USB on/off part in between is judged by Chrome's freeze count instead.
  const end = Date.now();
  for (const [part, from, to] of [['the takeovers', startedAt, takeoverEnd], ['the time without a session', usbEnd, end]]) {
    const freeze = video.between(from, to);
    t.diagnostic(`longest time without a video frame during ${part}: ${freeze.ms} ms`);
    assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms during ${part}`);
  }
  assert.ok(end - startedAt <= 420_000, 'the test takes 420 seconds at most');
});
