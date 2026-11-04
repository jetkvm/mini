import assert from 'node:assert/strict';
import { after, test } from 'node:test';
import { hostAgent } from '../lib/host.mjs';
import { spaceReachesHost } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';

// The Mini wakes a suspended (S3) Linux host only via wakeHost, not via input
// reports. Each suspend sets a 90 s RTC alarm as a safety net; the remote
// agent not answering means asleep.

const INPUT_DEVICES = ['keyboard', 'absolute_mouse', 'relative_mouse'];
// The host's alarm, in seconds after the suspend.
const WAKE_AFTER_SECONDS = 90;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// "<file> <value>" for each USB device of the host that can be set to wake it.
const WAKE_FLAGS = 'for d in /sys/bus/usb/devices/*/; do [ -f "$d/power/wakeup" ] && echo "${d}power/wakeup $(cat "$d/power/wakeup")"; done; true';
// No USB device of the host may wake it but the Mini, so that the test does
// not pass by other USB traffic. Fails when the host has no USB device of the
// Mini.
const ONLY_THE_MINI_WAKES =
  'found=0; ' +
  'for d in /sys/bus/usb/devices/*/; do ' +
  '[ -f "$d/power/wakeup" ] && echo disabled | sudo tee "$d/power/wakeup" > /dev/null || true; ' +
  'done; ' +
  'for d in /sys/bus/usb/devices/*/; do ' +
  'if [ -f "$d/power/wakeup" ] && cat "$d/product" "$d/manufacturer" 2>/dev/null | grep -q JetKVM; then ' +
  'echo enabled | sudo tee "$d/power/wakeup" > /dev/null; ' +
  'found=1; ' +
  'fi; done; ' +
  '[ "$found" -eq 1 ]';
// rtcwake suspends the host and sets the alarm. It starts half a second
// later, so that the SSH session of this command has ended by then.
const SUSPEND = `sudo sh -c 'nohup sh -c "sleep 0.5 && rtcwake -m mem -s ${WAKE_AFTER_SECONDS}" >/dev/null 2>&1 &'`;
// GNOME turns the screen on again; a host without GNOME has no such call.
const SCREEN_ON =
  'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus ' +
  'gdbus call --session --dest org.gnome.ScreenSaver ' +
  '--object-path /org/gnome/ScreenSaver ' +
  '--method org.gnome.ScreenSaver.SetActive false';

// Whether a version "1.2.3" or "1.2.3-dev..." is at least `minimum`.
function atLeast(version, minimum) {
  const have = version.replace(/^v/, '').split('-')[0].split('.').map(Number);
  const need = minimum.split('.').map(Number);
  for (let i = 0; i < 3; i++) {
    if ((have[i] ?? 0) !== (need[i] ?? 0)) return (have[i] ?? 0) > (need[i] ?? 0);
  }
  return true;
}

// One connection for the file, made by the first test that runs, `t`: the
// Mini with the USB devices of the host tests, a host that has them, video,
// and a first key. Without a Linux host there is no connection.
let session;
const start = t => (session ??= (async () => {
  const host = await hostAgent(t);
  if (!host) return { skip: true };
  const mini = await connect();
  // A start that fails leaves no browser behind.
  let restoreUsb = null;
  try {
    // Before the Mini's USB changes: the host lists the Mini again then,
    // with the wake setting of a new device.
    const wakeFlags = await host.exec(WAKE_FLAGS);
    restoreUsb = await hostTestUsb(mini);
    await host.inputDevices(INPUT_DEVICES, 30);
    await host.exec(SCREEN_ON).catch(() => undefined);
    // A session that began before the host had a picture gets no video.
    await until(mini.frames, count => count > 0, 30).catch(async () => {
      await mini.open();
      await until(mini.frames, count => count > 0, 30);
    });
    // The host opens the Mini's keyboard some time after it lists it.
    await spaceReachesHost(mini, host, 15);
    return { mini, host, restoreUsb, wakeFlags, suspended: false };
  } catch (error) {
    await (restoreUsb?.() ?? Promise.resolve()).catch(() => undefined);
    await mini.close().catch(() => undefined);
    throw error;
  }
})());

// The bench is given back with a host that is awake and reachable, with the
// Mini's USB state as found, and with the wake settings of the host's USB
// devices as found. Every step is tried, also after one that failed.
after(async () => {
  if (!session) return;
  const { mini, host, restoreUsb, wakeFlags, suspended } = await session.catch(() => ({}));
  if (!mini) return;
  const errors = [];
  const step = action => action().catch(error => errors.push(error));
  await step(async () => {
    // The alarm wakes a host that the Mini did not wake.
    if (suspended) await until(host.answers, Boolean, WAKE_AFTER_SECONDS + 30);
    await until(() => host.exec('true'), () => true, 60);
  });
  await step(restoreUsb);
  await step(async () => {
    const files = flags => flags.split('\n').filter(Boolean).map(line => line.split(' ')[0]).sort().join();
    // The host lists the Mini again after its USB state was stored.
    const now = await until(() => host.exec(WAKE_FLAGS), flags => files(flags) === files(wakeFlags), 15).catch(() => host.exec(WAKE_FLAGS));
    for (const line of wakeFlags.split('\n').filter(found => found && !now.split('\n').includes(found))) {
      const [file, value] = line.split(' ');
      // A device that the host lists at another place has another file.
      await host.exec(`[ ! -f ${file} ] || echo ${value} | sudo tee ${file} > /dev/null`);
    }
  });
  await step(() => mini.close());
  if (errors.length) throw new AggregateError(errors, `the bench is not as it was found: ${errors.map(error => error.message).join('; ')}`);
});

// The host is awake again, and the page has a session with video. A host that
// the Mini did not wake is woken by its alarm.
async function recover(shared) {
  const { mini, host } = shared;
  await until(host.answers, Boolean, 120);
  shared.suspended = false;
  await mini.open();
  await until(mini.frames, count => count > 0, 30);
}

// Suspends the host and returns when the host sleeps and the Mini reports no
// HDMI signal. Returns false, with the test skipped, on a host that cannot
// do it.
async function suspendHost(t, shared) {
  const { mini, host } = shared;
  const memSleep = await host.exec('cat /sys/power/mem_sleep').then(text => text.trim(), () => null);
  if (memSleep === null) {
    t.skip('the host does not tell its sleep states (/sys/power/mem_sleep)');
    return false;
  }
  if (!memSleep.includes('deep') && !memSleep.includes('[mem]')) {
    t.skip(`the host has no suspend-to-RAM (mem_sleep: ${memSleep})`);
    return false;
  }
  // As root, as for the suspend: rtcwake can be outside the user's PATH.
  if (!(await host.exec('sudo -n rtcwake --version >/dev/null').then(() => true, () => false))) {
    t.skip('the host has no rtcwake for the alarm that wakes it');
    return false;
  }
  const { systemVersion } = await mini.rpc('getLocalVersion');
  if (!atLeast(systemVersion, '0.2.8')) {
    t.skip(`a wake from S3 needs the system 0.2.8 (it is ${systemVersion})`);
    return false;
  }

  await host.exec(ONLY_THE_MINI_WAKES);
  await until(mini.frames, count => count > 0, 10);
  assert.equal(await host.answers(), true, 'the host is awake before the test suspends it');

  shared.suspended = true;
  // The SSH session can end with the suspend.
  await host.exec(SUSPEND, 5000).catch(() => undefined);
  try {
    await until(host.answers, answers => !answers, 15);
    await until(() => mini.rpc('getVideoState'), state => state.error === 'no_signal', 20);
  } catch (error) {
    await recover(shared);
    throw error;
  }
  return true;
}

// Asks the Mini to wake the host, again every 2 seconds, until the host's
// agent answers, for 30 seconds at most. Returns the time that it took.
async function wakeHost(shared) {
  const { mini, host } = shared;
  const askedAt = Date.now();
  await mini.rpc('wakeHost');
  const end = Date.now() + 30_000;
  let up = false;
  try {
    while (Date.now() < end) {
      if (await host.answers()) {
        up = true;
        shared.suspended = false;
        break;
      }
      // The first signal can be lost, and so can the call's answer.
      await mini.rpc('wakeHost').catch(() => undefined);
      await sleep(2000);
    }
  } finally {
    if (!up) await recover(shared);
  }
  assert.equal(up, true, 'the host is awake within 30 seconds of the call wakeHost');
  return Date.now() - askedAt;
}

// After the wake: a new session has video within 30 seconds, and the host
// gets a key from the Mini's keyboard within 30 seconds.
async function videoAndKeysAreBack({ mini, host }) {
  await sleep(5000);
  const openedAt = Date.now();
  await mini.open();
  assert.ok(Date.now() - openedAt <= 30_000, 'the page has a session with the RPC within 30 seconds');
  await until(mini.frames, count => count > 0, 30);
  const presses = await spaceReachesHost(mini, host, 30);
  assert.ok(presses.length > 0, 'the host gets a key from the Mini after the wake');
}

// A Linux host with suspend-to-RAM and rtcwake.
test('the Mini wakes a host that sleeps when it gets the call wakeHost', { timeout: 600_000 }, async t => {
  const shared = await start(t);
  // hostAgent() has skipped the first test with the reason.
  if (shared.skip) return t.skip('needs a Linux host with the remote agent');
  const startedAt = Date.now();
  if (!(await suspendHost(t, shared))) return;

  const wakeMs = await wakeHost(shared);
  t.diagnostic(`the host answered ${wakeMs} ms after the call wakeHost`);
  await videoAndKeysAreBack(shared);
  assert.ok(Date.now() - startedAt <= 180_000, 'the test takes 180 seconds at most');
});

// A Linux host with suspend-to-RAM and rtcwake.
test('mouse and keyboard reports do not wake a host that sleeps, and the call wakeHost then does', { timeout: 600_000 }, async t => {
  const shared = await start(t);
  // hostAgent() has skipped the first test with the reason.
  if (shared.skip) return t.skip('needs a Linux host with the remote agent');
  const { mini, host } = shared;
  const startedAt = Date.now();
  if (!(await suspendHost(t, shared))) return;

  // A move and a click of the relative mouse, and the space key.
  await mini.rpc('relMouseReport', { dx: 8, dy: 0, buttons: 0 });
  await mini.rpc('relMouseReport', { dx: 0, dy: 0, buttons: 1 });
  await mini.rpc('relMouseReport', { dx: 0, dy: 0, buttons: 0 });
  await mini.rpc('keyboardReport', { keys: [0x2c, 0, 0, 0, 0, 0], modifier: 0 });
  await mini.rpc('keyboardReport', { keys: [0, 0, 0, 0, 0, 0], modifier: 0 });
  await sleep(5000);
  if (await host.answers()) {
    shared.suspended = false;
    assert.fail('the host woke within 5 seconds of the mouse and keyboard reports');
  }

  const wakeMs = await wakeHost(shared);
  t.diagnostic(`the host answered ${wakeMs} ms after the call wakeHost`);
  await videoAndKeysAreBack(shared);
  assert.ok(Date.now() - startedAt <= 180_000, 'the test takes 180 seconds at most');
});
