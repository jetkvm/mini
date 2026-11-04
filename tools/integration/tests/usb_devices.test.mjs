import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hostAgent } from '../lib/host.mjs';
import { spaceReachesHost } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';
import { MAX_FREEZE_MS, hdmiCapture, watchVideo } from '../lib/video.mjs';

const INPUT_DEVICES = ['keyboard', 'absolute_mouse', 'relative_mouse'];
// The vendor and product IDs of the Mini's default USB identity, as the host lists them.
const ID_DEFAULT = '399b:0001';
// The identity of the page's "Logitech" preset.
const LOGITECH = {
  vendor_id: '0x046d',
  product_id: '0xc52b',
  serial_number: '1234567&0&1',
  manufacturer: 'Logitech (x64)',
  product: 'Logitech USB Input Device',
};
const ID_LOGITECH = '046d:c52b';

// Each change of the Mini's USB devices or of its USB identity takes the Mini
// off the host's USB and puts it on again with the new descriptors. A Linux
// host.
test('the host sees the Mini\'s USB devices and its USB identity after each change', { timeout: 420_000 }, async t => {
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
  restoreUsb = await hostTestUsb(mini);
  await hdmiCapture(mini);
  await host.inputDevices(INPUT_DEVICES, 30);
  // The host has opened the keyboard that it lists.
  assert.ok((await spaceReachesHost(mini, host, 15)).length > 0, 'the host gets a key before the test');
  // The watch judges a video that runs.
  await until(mini.frames, count => count > 0, 30);
  const startedAt = Date.now();
  const video = watchVideo(mini);
  t.after(video);

  const usbDevices = async () => (await host.get('/usb/devices')) ?? [];
  // The host can miss a change of the Mini's USB, and more time does not
  // help then: the call is sent again every 15 seconds, for 45 seconds.
  const change = async (method, params, reached) => {
    const end = Date.now() + 45_000;
    for (;;) {
      await mini.rpc(method, params).catch(e => {
        if (!/RPC timeout/.test(e.message)) throw e;
      });
      try {
        return await reached(Math.max(Math.min(15, (end - Date.now()) / 1000), 1));
      } catch (e) {
        if (Date.now() >= end) throw e;
      }
    }
  };
  const hostHas = id => seconds => until(async () => (await usbDevices()).filter(device => device.id === id), found => found.length > 0, seconds);

  const usb = (await usbDevices()).find(device => device.name.includes('JetKVM') || device.id === ID_DEFAULT);
  assert.ok(usb, 'the host has the Mini as a USB device');
  assert.match(usb.name, /JetKVM/);
  assert.equal(usb.id, ID_DEFAULT);
  // Exactly the three input devices, as the wait before found them.
  await host.inputDevices(INPUT_DEVICES, 1);

  const devices = await mini.rpc('getUsbDevices');
  const config = await mini.rpc('getUsbConfig');
  await change('setUsbDevices', { devices: { ...devices, absolute_mouse: false, relative_mouse: false, mass_storage: false } },
    seconds => host.inputDevices(['keyboard'], seconds));
  await change('setUsbDevices', { devices }, seconds => host.inputDevices(INPUT_DEVICES, seconds));

  const logitech = await change('setUsbConfig', { usbConfig: LOGITECH }, hostHas(ID_LOGITECH));
  assert.match(logitech[0].name, /Logitech/, 'the host has the new manufacturer and product names');
  await change('setUsbConfig', { usbConfig: config }, hostHas(ID_DEFAULT));

  const freeze = video();
  t.diagnostic(`longest time without a video frame ${freeze.ms} ms`);
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
  assert.ok(Date.now() - startedAt <= 240_000, 'the test takes 240 seconds at most');
});
