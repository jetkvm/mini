import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hostAgent } from '../lib/host.mjs';
import { keyboard, spaceReachesHost } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';

const INPUT_DEVICES = ['keyboard', 'absolute_mouse', 'relative_mouse'];
const HID_SPACE = 0x2c;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// The host's name of the USB interface of the Mini's keyboard, for example
// "1-2:1.0": HID class (03) with the boot keyboard protocol (01), on a device
// whose manufacturer is JetKVM.
const KEYBOARD_INTERFACE =
  'for d in /sys/bus/usb/devices/*-*/; do ' +
  '[ -f "$d/manufacturer" ] || continue; ' +
  'grep -qi jetkvm "$d/manufacturer" 2>/dev/null || continue; ' +
  'for i in "$d"*:*/; do ' +
  '[ -f "$i/bInterfaceClass" ] || continue; ' +
  '[ "$(cat "$i/bInterfaceClass")" = "03" ] || continue; ' +
  '[ "$(cat "$i/bInterfaceProtocol")" = "01" ] && basename "$i"; ' +
  'done; ' +
  'done';

// With the host's HID driver unbound, keyboard writes time out; the Mini must
// reconnect USB by itself so keys work again. Linux host.
test('the keyboard works again by itself after the host has stopped reading it', { timeout: 600_000 }, async t => {
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
  await host.inputDevices(INPUT_DEVICES, 30);

  const startedAt = Date.now();
  assert.ok((await spaceReachesHost(mini, host, 30)).length > 0, 'the host gets a key before the test');
  const devices = await mini.rpc('getUsbDevices');
  const iface = (await host.exec(KEYBOARD_INTERFACE)).trim().split('\n')[0] ?? '';
  assert.match(iface, /^[\d.-]+:\d+\.\d+$/, 'the host has the USB interface of the Mini\'s keyboard');
  // The driver's files take a write of root alone.
  const unbind = `if [ "$(id -u)" = 0 ]; then echo -n "${iface}" > /sys/bus/usb/drivers/usbhid/unbind; else echo -n "${iface}" | sudo -n tee /sys/bus/usb/drivers/usbhid/unbind > /dev/null; fi`;
  const bind = `if [ "$(id -u)" = 0 ]; then echo -n "${iface}" > /sys/bus/usb/drivers/usbhid/bind 2>/dev/null || true; else echo -n "${iface}" | sudo -n tee /sys/bus/usb/drivers/usbhid/bind > /dev/null 2>&1 || true; fi`;

  try {
    await host.exec(unbind, 10_000);
    const unboundAt = Date.now();

    // Eight keys that the host does not read.
    const keys = await keyboard(mini);
    for (let i = 0; i < 8; i++) {
      await keys.press(HID_SPACE, true);
      await sleep(20);
      await keys.press(HID_SPACE, false);
      await sleep(250);
    }

    const presses = await spaceReachesHost(mini, host, 120);
    t.diagnostic(`the host got a key again ${Date.now() - unboundAt} ms after its driver let go of the keyboard`);
    assert.ok(presses.length > 0, 'the host gets a key again within 120 seconds, without a restart of the Mini');

    if (devices.mass_storage) {
      // The interfaces alone do not show it: after the Mini has connected
      // its USB again the keyboard can work while the drive's first SCSI
      // command gets no answer. The host has a block device only when the
      // drive answers.
      const drive = () => host.exec('lsblk -SJ -o NAME,VENDOR,TRAN').then(text =>
        JSON.parse(text).blockdevices.some(device => device.tran === 'usb' && device.vendor?.trim() === 'JetKVM'));
      await until(drive, found => found, 15).catch(e => assert.fail(`the host has the Mini's drive within 15 seconds: ${e.message}`));
    }
  } finally {
    // For a Mini that did not recover: the host's driver takes the keyboard again.
    await host.exec(bind, 10_000);
  }
  assert.ok(Date.now() - startedAt <= 240_000, 'the test takes 240 seconds at most');
});
