import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connectHost } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';

// The Mini's USB Serial/JTAG debug interface, a second USB device on a host
// that the Mini's Power/EXT port connects to. Its serial number is the
// Mini's device ID as a MAC address.
const DEBUG_INTERFACE = '303a:1001';
const debugSerials = async host => (await host.exec(
  'for d in /sys/bus/usb/devices/*; do ' +
  '[ "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" = 303a:1001 ] || continue; ' +
  'cat "$d/serial" 2>/dev/null; done')).split('\n').map(line => line.trim().toUpperCase()).filter(Boolean);

// One restart that the firmware asks for; the RPC is back afterwards. The
// Mini can restart before its answer arrives.
async function restart(mini) {
  const askedAt = Date.now();
  await mini.rpc('reboot', { force: true }, 10_000).catch(() => undefined);
  await until(() => mini.restarted(askedAt).catch(() => false), restarted => restarted, 120);
  await mini.open();
}

// Takes effect at the next restart. If the interface does not come back, the
// Mini must be replugged before the next flash.
test('USB debugging turns the debug interface off and on again at a restart', { timeout: 600_000 }, async t => {
  const mini = await connect();
  let restore = null;
  t.after(async () => {
    try {
      await restore?.();
    } finally {
      await mini.close();
    }
  });
  const capabilities = await mini.rpc('getDeviceCapabilities');
  if (!capabilities.includes('usb_debugging')) {
    t.skip('the Mini has no USB debugging setting');
    return;
  }
  const host = await connectHost(mini);
  if (host.system !== 'linux') {
    t.skip(`the bench's host is ${host.system}: this needs a Linux host`);
    return;
  }
  const serial = (await mini.rpc('getDeviceID')).match(/../g).join(':').toUpperCase();
  const listed = async () => (await debugSerials(host)).filter(found => found === serial).length;
  const found = await mini.rpc('getUsbDebuggingState');
  if (!found.active || (await listed()) !== 1) {
    t.skip(`${host.login} does not have the Mini's debug interface ${DEBUG_INTERFACE}`);
    return;
  }
  // The setting as found, with the interface connected.
  restore = async () => {
    const now = await mini.rpc('getUsbDebuggingState');
    if (now.enabled !== found.enabled) await mini.rpc('setUsbDebuggingState', { enabled: found.enabled });
    if (now.active !== found.active) await restart(mini);
  };

  assert.deepEqual(await mini.rpc('setUsbDebuggingState', { enabled: false }), { enabled: false, active: true },
    'off is stored and takes effect at the next restart');
  await restart(mini);
  await until(listed, count => count === 0, 30);
  assert.deepEqual(await mini.rpc('getUsbDebuggingState'), { enabled: false, active: false });

  assert.deepEqual(await mini.rpc('setUsbDebuggingState', { enabled: true }), { enabled: true, active: false },
    'on is stored and takes effect at the next restart');
  await restart(mini);
  await until(listed, count => count === 1, 30);
  assert.deepEqual(await mini.rpc('getUsbDebuggingState'), { enabled: true, active: true });
});
