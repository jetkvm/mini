import assert from 'node:assert/strict';
import { lookup } from 'node:dns/promises';
import { test } from 'node:test';
import { quote } from '../../combined_load/runner/hosts.mjs';
import { approveMini } from '../lib/approval.mjs';
import { connectHost, ntpResponder } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';

// The jiggler moves the mouse every second, after one second without input,
// with no random delay.
const JIGGLER = { inactivity_limit_seconds: 1, jitter_percentage: 0, schedule_cron_tab: '* * * * * *', timezone: 'UTC' };

// --- OS Services: the sender on a Linux host ---

const SENDER_UNIT = 'ipkvm-os-services.service';
const FINGERPRINT = /^(?:[0-9A-F]{2}:){7}[0-9A-F]{2}$/;
const OS_SERVICES_INTERFACE = 'ff4a01';
// pageRpc joins an error's message and data, and the page's stack follows on
// the next lines; "Internal error" is the message of the code -32603.
const CLIPBOARD_OFF = /: guestClipboardGet: Internal error: clipboard sharing is turned off in the OS Services settings$/m;
// The systemd user manager of the SSH login's user.
const USER_SESSION =
  'export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"; ' +
  'export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"; ';
// One line per device, interface and endpoint with the vendor:product ID $1.
const USB_LISTING =
  'r() { cat "$1" 2>/dev/null | tr -d "\\t\\n"; }; ' +
  'for d in /sys/bus/usb/devices/*; do ' +
  '[ -f "$d/idVendor" ] || continue; ' +
  '[ "$(r "$d/idVendor"):$(r "$d/idProduct")" = "$id" ] || continue; ' +
  'n=${d##*/}; ' +
  'printf "device\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\n" "$n" "$(r "$d/version" | tr -d " ")" "$(r "$d/bcdDevice")" ' +
  '"$(r "$d/manufacturer")" "$(r "$d/product")" "$(r "$d/serial")"; ' +
  'for i in "$d/$n":*; do ' +
  '[ -f "$i/bInterfaceClass" ] || continue; ' +
  'printf "interface\\t%s\\t%s\\n" "$(r "$i/bInterfaceNumber")" ' +
  '"$(r "$i/bInterfaceClass")$(r "$i/bInterfaceSubClass")$(r "$i/bInterfaceProtocol")"; ' +
  'for e in "$i"/ep_*; do [ -d "$e" ] || continue; ' +
  'printf "endpoint\\t%s\\t%s\\t%s\\t%s\\n" "$(r "$e/bEndpointAddress")" "$(r "$e/type")" ' +
  '"$(r "$e/direction")" "$(r "$e/wMaxPacketSize")"; ' +
  'done; done; done';

const hex4 = value => Number.parseInt(value, 16).toString(16).padStart(4, '0');

// The Mini's USB Serial/JTAG debug interface, which is a second USB device
// on a host that the Mini's debug port connects to. Its serial number is the
// Mini's device ID as a MAC address.
const DEBUG_INTERFACE = '303a:1001';
const debugInterfaces = async (host, serial) => (await host.exec(`id=${DEBUG_INTERFACE}; ${USB_LISTING}`)).split('\n')
  .filter(line => line.startsWith('device\t') && line.split('\t')[6].toUpperCase() === serial).length;

async function senderUnit(host) {
  const text = await host.exec(USER_SESSION + `systemctl --user show -p LoadState,ActiveState ${SENDER_UNIT}`);
  return Object.fromEntries(text.trim().split('\n').map(line => line.split('=')));
}

// The Mini's USB identity on the host, without bus and device numbers.
async function usbIdentity(mini, host) {
  const config = await mini.rpc('getUsbConfig');
  const id = `${hex4(config.vendor_id)}:${hex4(config.product_id)}`;
  const listing = await until(() => host.exec(`id=${quote(id)}; ${USB_LISTING}`), text => text.trim() !== '', 30);
  const devices = [];
  for (const line of listing.split('\n').filter(Boolean)) {
    const [kind, ...fields] = line.split('\t');
    if (kind === 'device') {
      const [name, version, bcdDevice, manufacturer, product, serial] = fields;
      devices.push({ name, id, version, bcdDevice, manufacturer, product, serial, interfaces: [] });
    } else if (kind === 'interface') {
      devices.at(-1).interfaces.push({ number: Number.parseInt(fields[0], 16), triple: fields[1].toLowerCase(), endpoints: [] });
    } else if (kind === 'endpoint') {
      const [address, type, direction, maxPacketSize] = fields;
      devices.at(-1).interfaces.at(-1).endpoints.push(`${address} ${type} ${direction} ${Number.parseInt(maxPacketSize, 16)}`);
    }
  }
  assert.equal(devices.length, 1, `${host.login} has one USB device ${id}: ${devices.map(device => device.name)}`);
  const [{ name: _name, ...device }] = devices;
  assert.ok(device.interfaces.filter(i => i.triple === OS_SERVICES_INTERFACE).length <= 1,
    `interfaces ${OS_SERVICES_INTERFACE} of ${id}: ${JSON.stringify(device.interfaces)}`);
  device.interfaces.sort((a, b) => a.number - b.number);
  return device;
}

// The session is open and approved.
const connected = state => state.osServices.connected && !state.osServices.approvalPending;
// Also the clipboard is offered and the sender's details are published.
const connectedWithClipboard = state =>
  connected(state) && state.guestClipboard !== undefined && Boolean(state.osServices.sender?.version);

// --- Wake-on-LAN ---

const wakeTargets = value => {
  if (!Array.isArray(value)) throw new Error(`getWakeOnLanDevices: ${JSON.stringify(value)}`);
  return value.map(item => ({
    name: String(item.name ?? ''),
    macAddress: String(item.macAddress ?? ''),
    ...(item.broadcastIP ? { broadcastIP: String(item.broadcastIP) } : {}),
  }));
};

// --- The reboot ---

// esp_reset_reason(): a restart that the firmware asked for.
const ESP_RST_SW = 3;

const videoStats = async mini => {
  const response = await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(3000) });
  if (!response.ok) throw new Error(`/webrtc/stats: ${response.status}`);
  return response.json();
};

/**
 * Reboots over RPC, calls whenDown() once the Mini is down, and reopens.
 * Returns { answer, notice, backAt }; answer and the willReboot notice carry
 * their `position` among received messages.
 */
async function reboot(mini, whenDown) {
  const askedAt = Date.now();
  const id = await mini.send('reboot', { force: true });
  const messages = await until(() => mini.received(),
    all => all.some(message => message.id === id) && all.some(message => message.method === 'willReboot'), 10);
  const placed = find => {
    const position = messages.findIndex(find);
    return { position, message: messages[position] };
  };
  const answer = placed(message => message.id === id);
  const notice = placed(message => message.method === 'willReboot');
  // Down: three requests in a row without an answer. One miss is no reboot.
  // Three misses (~1.25 s each) fit in the few seconds an Ethernet Mini is
  // away for a reboot.
  let misses = 0;
  await until(async () => {
    const up = await fetch(new URL('/device/status', mini.url), { signal: AbortSignal.timeout(1000) }).then(r => r.ok, () => false);
    misses = up ? 0 : misses + 1;
    return misses;
  }, count => count >= 3, 30).catch(e => {
    throw new Error(`the Mini did not go down after the reboot request: ${e.message}`);
  });
  const downAt = Date.now();
  whenDown();
  await mini.open();
  const backAt = Date.now();
  const seconds = (backAt - downAt) / 1000;
  assert.ok(seconds <= 90, `the RPC was back ${seconds.toFixed(0)} s after the Mini went down; the limit is 90 s`);
  assert.equal(await mini.restarted(askedAt), true, 'the Mini has restarted');
  return { answer, notice, backAt };
}

// One reboot, one subtest per setting so a failure names it. Subtest
// deadlines count from the reboot.
test('settings survive a reboot', { timeout: 600_000 }, async t => {
  const mini = await connect();
  const after = [];
  // Also when the Mini does not come back: an open browser or a running
  // command on the host keeps the test's process alive. close() stores the
  // Mini's settings as connect() found them.
  t.after(async () => {
    const errors = [];
    for (const step of [...after.reverse(), () => mini.close()]) await step().catch(e => errors.push(e));
    if (errors.length) throw new AggregateError(errors, errors.map(e => e.message).join('; '));
  });
  const host = await connectHost(mini);
  const stamp = new Date().toISOString().replace(/\D/g, '');
  const state = () => mini.rpc('getVideoState');

  // Video is HDMI capture, which the Mini has from its start; service video
  // comes back only when the sender has connected again. close() stores the
  // setting as found.
  if ((await mini.rpc('getOsServicesSettings')).video_enabled) {
    await mini.rpc('setOsServicesSettings', { settings: { video_enabled: false } });
    await until(state, now => now.source === 'physical', 45);
  }

  // The keyboard macros: one more, with one step that presses F24.
  const macrosFound = await mini.rpc('getKeyboardMacros');
  const macro = {
    id: stamp.slice(-7),
    name: `Integration F24 ${stamp}`,
    steps: [{ keys: ['F24'], modifiers: [], delay: 50 }],
    sortOrder: macrosFound.length + 1,
  };
  await mini.rpc('setKeyboardMacros', { params: { macros: [...macrosFound, macro] } });
  const macros = await mini.rpc('getKeyboardMacros');
  assert.deepEqual(macros.filter(stored => stored.name === macro.name).map(stored => stored.steps), [macro.steps], 'the macro is stored');

  // The jiggler: on, with a schedule of its own, in the order of the page's "Save Jiggler Config".
  const jiggler = async () => ({ state: await mini.rpc('getJigglerState'), config: await mini.rpc('getJigglerConfig') });
  await mini.rpc('setJigglerState', { enabled: true });
  await mini.rpc('setJigglerConfig', { jigglerConfig: JIGGLER });
  await until(jiggler, now => assert.deepEqual(now, { state: true, config: JIGGLER }) ?? true, 12);

  // Wake-on-LAN: one more target, with a locally administered unicast MAC
  // and a broadcast address of the documentation range. No packet is sent.
  const targetsFound = wakeTargets(await mini.rpc('getWakeOnLanDevices'));
  assert.ok(targetsFound.length < 16, 'the Mini has room for one more Wake-on-LAN target');
  const targets = [...targetsFound, {
    name: `Integration ${stamp.slice(4, 8)}-${stamp.slice(8, 14)}`,
    macAddress: `02:16:07:${stamp.slice(8, 10)}:${stamp.slice(10, 12)}:${stamp.slice(12, 14)}`,
    broadcastIP: '192.0.2.255',
  }];
  await mini.rpc('setWakeOnLanDevices', { params: { devices: targets } });
  assert.deepEqual(wakeTargets(await mini.rpc('getWakeOnLanDevices')), targets, 'the target is stored');

  // The stream quality: Auto (0), or Medium (0.5) on a Mini that has Auto,
  // which is also what a Mini without a stored quality has.
  const quality = (await mini.rpc('getStreamQualityFactor')) === 0 ? 0.5 : 0;
  await mini.rpc('setStreamQualityFactor', { factor: quality });
  await until(() => mini.rpc('getStreamQualityFactor'), now => now === quality, 5);

  // Time: the host as the one NTP server, without the public servers. A
  // Linux host answers the Mini's requests and counts them.
  const linux = host.system === 'linux';
  const needsLinux = linux ? false : `the bench's host is ${host.system}: this needs a Linux host`;
  const time = { time_sync_mode: 'custom', time_sync_ntp_servers: [host.login.split('@').at(-1)], time_sync_disable_fallback: true };
  const timeOf = ({ time_sync_mode, time_sync_ntp_servers, time_sync_disable_fallback }) =>
    ({ time_sync_mode, time_sync_ntp_servers, time_sync_disable_fallback });
  let ntp = null;
  if (linux) {
    ntp = await ntpResponder(host, (await lookup(new URL(mini.url).hostname, { family: 4 })).address);
    after.push(() => ntp.stop());
  }
  await mini.rpc('setNetworkSettings', { settings: { ...(await mini.rpc('getNetworkSettings')), ...time } });
  await until(() => mini.rpc('getNetworkSettings'), now => assert.deepEqual(timeOf(now), time) ?? true, 5);

  // OS Services: the sender on the host runs, has approved the Mini and
  // offers the clipboard. Then five settings change.
  const unit = linux ? await senderUnit(host) : null;
  const noSender = needsLinux ||
    (unit.LoadState === 'loaded' ? false : `${host.login} has no ${SENDER_UNIT} user unit (LoadState ${unit.LoadState})`);
  let os = null;
  if (!noSender) {
    const ran = unit.ActiveState === 'active';
    let approval = null;
    // The host as found: the sender's list, and a sender that ran runs. A
    // sender that had not approved the Mini starts again without its session.
    after.push(async () => {
      await approval?.forget();
      if (!ran || approval?.approvedBefore === false) await host.exec(host.commands.stopSender, { timeoutMs: 45_000 });
      if (ran && approval?.approvedBefore === false) await host.exec(host.commands.startSender, { timeoutMs: 45_000 });
    });
    if (!ran) await host.exec(host.commands.startSender, { timeoutMs: 45_000 });
    approval = await approveMini(mini);
    const { fingerprint } = approval;
    assert.match(fingerprint, FINGERPRINT, 'the Mini\'s fingerprint');
    if (!(await mini.rpc('getOsServicesSettings')).clipboard_enabled) {
      await mini.rpc('setOsServicesSettings', { settings: { clipboard_enabled: true } });
    }
    await until(state, connectedWithClipboard, 45);

    const before = await mini.rpc('getOsServicesSettings');
    const changed = {
      terminal_enabled: !before.terminal_enabled,
      files_enabled: !before.files_enabled,
      clipboard_enabled: false,
      resolution: before.resolution === '1920x1080' ? '1280x720' : '1920x1080',
      adaptation: before.adaptation === 'fixed:8000:30' ? 'fixed:6000:24' : 'fixed:8000:30',
    };
    const applied = await mini.rpc('setOsServicesSettings', { settings: changed });
    assert.deepEqual(applied, { ...before, ...changed }, 'the changed OS Services settings');
    os = { fingerprint, before, applied, identity: await usbIdentity(mini, host) };
  }

  // The video codec preference, as found.
  const codec = await mini.rpc('getVideoCodecPreference');

  // The other names for the KVM, which the Host check takes: one more.
  const namesFound = await mini.rpc('getExtraHostnames');
  assert.ok(namesFound.length < 8, 'the Mini has room for one more name');
  after.push(() => mini.rpc('setExtraHostnames', { hostnames: namesFound }));
  const names = await mini.rpc('setExtraHostnames', { hostnames: [...namesFound, `kvm-${stamp.slice(-6)}.bench.example`] });

  // The video mode before the reboot.
  const { width, height } = await until(() => videoStats(mini), stats => stats.modeValid === true, 30);

  // The debug interface, on a Linux host that has it now.
  const debugSerial = (await mini.rpc('getDeviceID')).match(/../g).join(':').toUpperCase();
  const noDebugInterface = needsLinux ||
    ((await debugInterfaces(host, debugSerial)) === 1 ? false : `${host.login} does not have the Mini's debug interface ${DEBUG_INTERFACE}`);

  // The requests that the NTP server has counted when the Mini is down: a
  // later one is of the Mini after its reboot.
  let requestsBefore;
  const { answer, notice, backAt } = await reboot(mini, () => { requestsBefore = ntp?.count(); });
  // What the Mini does by itself within 90 seconds after the reboot.
  const settle = promise => promise.then(value => ({ value }), error => ({ error }));
  const asksAgain = ntp && settle(until(() => ntp.count(), count => count > requestsBefore, 90));
  const senderBack = os && settle(until(state, connected, 90));

  await t.test('the Mini answers the reboot request before it announces the reboot', async () => {
    assert.deepEqual(answer.message, { jsonrpc: '2.0', id: answer.message.id, result: null });
    assert.deepEqual(notice.message, { jsonrpc: '2.0', method: 'willReboot', params: null });
    assert.ok(answer.position < notice.position, `the answer is message ${answer.position} and willReboot is message ${notice.position}`);
  });

  await t.test('the restart is the firmware\'s, and the Mini runs the same image', async () => {
    const now = await mini.status();
    assert.equal(now.resetReason, ESP_RST_SW, 'esp_reset_reason()');
    assert.equal(now.firmware.elfSha256, mini.image.elfSha256);
  });

  await t.test('video returns in the same mode after the reboot', async () => {
    const stats = await until(() => videoStats(mini), now => now.modeValid === true && now.peerConnected === true, 60);
    assert.deepEqual({ width: stats.width, height: stats.height }, { width, height });
  });

  await t.test('the video codec preference survives the reboot', async () => {
    assert.deepEqual(await mini.rpc('getVideoCodecPreference'), codec);
  });

  await t.test('the other names for the KVM survive the reboot', async () => {
    assert.deepEqual(await mini.rpc('getExtraHostnames'), names);
  });

  await t.test('the debug interface is back on the host', { skip: noDebugInterface }, async () => {
    await until(() => debugInterfaces(host, debugSerial), count => count === 1, 30);
  });

  await t.test('the keyboard macros survive the reboot', async () => {
    assert.deepEqual(await mini.rpc('getKeyboardMacros'), macros);
  });

  await t.test('the jiggler and its schedule survive the reboot', async () => {
    assert.deepEqual(await jiggler(), { state: true, config: JIGGLER });
  });

  await t.test('the Wake-on-LAN target survives the reboot', async () => {
    assert.deepEqual(wakeTargets(await mini.rpc('getWakeOnLanDevices')), targets);
  });

  await t.test('the stream quality survives the reboot', async () => {
    assert.equal(await mini.rpc('getStreamQualityFactor'), quality);
  });

  await t.test('the custom NTP server survives the reboot', async () => {
    assert.deepEqual(timeOf(await mini.rpc('getNetworkSettings')), time);
  });

  await t.test('the Mini asks the custom NTP server again after the reboot', { skip: needsLinux }, async () => {
    const { error } = await asksAgain;
    if (error) throw error;
  });

  await t.test('the OS Services settings, the fingerprint and the approval survive the reboot', { skip: noSender }, async () => {
    try {
      assert.deepEqual(await mini.rpc('getOsServicesSettings'), os.applied, 'the settings after the reboot');
      const { value: back, error } = await senderBack;
      if (error) throw new Error(`the sender did not connect again on its own: ${error.message}`);
      assert.equal(back.osServices.fingerprint, os.fingerprint, 'the same key after the reboot');
      assert.equal(back.osServices.approvalPending, false, 'still approved');
      assert.equal(back.guestClipboard, undefined, 'clipboard sharing is still off');
      await assert.rejects(mini.rpc('guestClipboardGet'), CLIPBOARD_OFF);
      assert.deepEqual(await usbIdentity(mini, host), os.identity, 'the USB identity');
    } finally {
      await mini.rpc('setOsServicesSettings', { settings: { ...os.before, adaptation: os.before.adaptation ?? '' } });
    }
    assert.deepEqual(await mini.rpc('getOsServicesSettings'), os.before);
    await until(state, connectedWithClipboard, 30);
    // A text to the host's clipboard and back, as the walk's osClipboard
    // does. The sender offers the clipboard only when it can open it.
    const text = `after the reboot ${Date.now()} ✓`;
    assert.deepEqual(await mini.rpc('guestClipboardSet', { text }, 15_000), { status: 'ok' });
    assert.deepEqual(await mini.rpc('guestClipboardGet', {}, 15_000), { status: 'ok', text, truncated: false });
  });

  // Up since the reboot: a second restart would have set the uptime back.
  await t.test('the Mini went down once', async () => {
    assert.equal(await mini.restarted(backAt), false);
  });
});
