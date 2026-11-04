import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hostAgent } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';
import { MAX_FREEZE_MS, watchVideo } from '../lib/video.mjs';

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// A Linux host. The audio setting turns the sound that the Mini sends to the
// browser off and on. The Mini's USB sound device is not a part of it: the
// host keeps the device, and with it its choice of an output.
test('the host keeps the Mini\'s USB audio device when the sound to the browser is turned off, on and off', { timeout: 180_000 }, async t => {
  const host = await hostAgent(t);
  if (!host) return;
  const mini = await connect();
  let restoreUsb = null;
  t.after(async () => {
    try {
      await restoreUsb?.();
    } finally {
      await mini.close();
    }
  });
  restoreUsb = await hostTestUsb(mini, { audio: true });
  await host.inputDevices(['keyboard', 'absolute_mouse', 'relative_mouse'], 30);
  // The watch judges a video that runs.
  await until(mini.frames, count => count > 0, 30);
  const startedAt = Date.now();
  const video = watchVideo(mini);
  t.after(video);

  for (const enabled of [false, true, false]) {
    await mini.rpc('setAudioConfig', { params: { enabled } });
    assert.deepEqual(await mini.rpc('getAudioConfig'), { enabled });
    // The time in which a host would take a changed device away.
    await sleep(3000);
    const devices = await until(() => host.get('/audio/devices'), list => list.some(device => device.is_jetkvm), 15)
      .catch(() => host.get('/audio/devices'));
    assert.ok(devices.some(device => device.is_jetkvm),
      `the host lists the Mini's audio device with the sound ${enabled ? 'on' : 'off'}: ${JSON.stringify(devices)}`);
  }
  const freeze = video();
  t.diagnostic(`longest time without a video frame ${freeze.ms} ms`);
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
  assert.ok(Date.now() - startedAt <= 45_000, 'the test takes 45 seconds at most');
});
