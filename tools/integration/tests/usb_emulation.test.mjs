import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hostAgent } from '../lib/host.mjs';
import { spaceReachesHost } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';
import { MAX_FREEZE_MS, hdmiCapture, watchVideo } from '../lib/video.mjs';

const INPUT_DEVICES = ['keyboard', 'absolute_mouse', 'relative_mouse'];

// The Mini and the bench's host with the USB devices of the host tests, and
// a video watch that has begun. The cleanup stores the USB state as found.
async function bench(t) {
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
  await hdmiCapture(mini);
  await host.inputDevices(INPUT_DEVICES, 30);
  // The host has opened the keyboard that it lists.
  assert.ok((await spaceReachesHost(mini, host, 15)).length > 0, 'the host gets a key before the test');
  // The watch judges a video that runs.
  await until(mini.frames, count => count > 0, 30);
  const video = watchVideo(mini);
  t.after(video);
  return { host, mini, video };
}

// The end of a test: the video did not stand still during it.
function videoRan(t, video) {
  const freeze = video();
  t.diagnostic(`longest time without a video frame ${freeze.ms} ms`);
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
}

// "USB Emulation" of the page's hardware settings: off takes the Mini off
// the host's USB, as a pulled cable does. A Linux host.
test('USB off removes the Mini\'s input devices from the host, and USB on brings the keyboard back', { timeout: 180_000 }, async t => {
  const found = await bench(t);
  if (!found) return;
  const { host, mini, video } = found;
  const startedAt = Date.now();

  await mini.rpc('setUsbEmulationState', { enabled: false });
  assert.equal(await mini.rpc('getUsbEmulationState'), false);
  await host.inputDevices([], 15);
  // Off once more, while it is off, changes nothing and is no error.
  await mini.rpc('setUsbEmulationState', { enabled: false });
  await mini.rpc('setUsbEmulationState', { enabled: true });
  assert.equal(await mini.rpc('getUsbEmulationState'), true);
  assert.ok((await spaceReachesHost(mini, host, 30)).length > 0, 'the host gets a key within 30 seconds after USB is on again');

  videoRan(t, video);
  assert.ok(Date.now() - startedAt <= 90_000, 'the test takes 90 seconds at most');
});

// The Caps Lock LED of the Mini's keyboard on the host, as
// "<path> <brightness>" per LED.
const CAPS_LOCK_LEDS =
  'for p in /sys/class/leds/*::capslock; do [ -e "$p" ] || continue; ' +
  'grep -q JetKVM "$p/device/name" 2>/dev/null && echo "$p $(cat "$p/brightness")"; done; true';

// Enabling USB while on must neither drop the LED report receiver nor
// re-enumerate. No keys are sent: a key could reopen the keyboard and hide a
// lost receiver. Linux host.
test('the host\'s Caps Lock state still reaches the Mini after USB is turned on twice while it is on', { timeout: 120_000 }, async t => {
  const found = await bench(t);
  if (!found) return;
  const { host, mini, video } = found;
  const startedAt = Date.now();

  const leds = (await host.exec(CAPS_LOCK_LEDS)).trim().split('\n').filter(Boolean).map(line => line.split(' '));
  assert.equal(leds.length, 1, 'the host has exactly one Caps Lock LED of the Mini\'s keyboard');
  const [path, brightness] = leds[0];
  assert.match(path, /^\/sys\/class\/leds\/input\d+::capslock$/);
  const lit = brightness !== '0';
  // The LED's file takes a write of root alone.
  const setHostLed = on => host.exec(`echo ${Number(on)} | sudo -n tee '${path}/brightness' > /dev/null`);
  // A failure names the host's own state then: a host that has put its LED
  // back has not lost a report on the way to the Mini.
  const miniReports = on => until(() => mini.rpc('getKeyboardLedState'), state => state.caps_lock === on, 5)
    .catch(async e => assert.fail(`the Mini reports Caps Lock ${on ? 'on' : 'off'} within 5 seconds: ${e.message}; ` +
      `the host's LED file reads ${(await host.exec(`cat '${path}/brightness' 2>&1; true`)).trim()}`));
  try {
    // The host's state reaches the Mini before the calls.
    await setHostLed(!lit);
    await miniReports(!lit);
    await setHostLed(lit);
    await miniReports(lit);

    await mini.rpc('setUsbEmulationState', { enabled: true });
    await mini.rpc('setUsbEmulationState', { enabled: true });

    // The same LED file: after a new enumeration the host has another one.
    await setHostLed(!lit);
    await miniReports(!lit);
  } finally {
    await setHostLed(lit).catch(() => undefined);
  }

  videoRan(t, video);
  assert.ok(Date.now() - startedAt <= 60_000, 'the test takes 60 seconds at most');
});
