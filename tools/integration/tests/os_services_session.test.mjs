import assert from 'node:assert/strict';
import fs from 'node:fs';
import { after, before, describe, test } from 'node:test';
import { quote } from '../../combined_load/runner/hosts.mjs';
import { sender } from '../lib/approval.mjs';
import { hostClipboard } from '../lib/host_clipboard.mjs';
import { connect, until } from '../lib/mini.mjs';
import { connected, connectedWithClipboard, openSession, rpcAnswer, videoState } from '../lib/os_services.mjs';
import { FINGERPRINT, SENDER_NOT_APPROVED, SENDER_SESSION_READY, senderHost } from '../lib/sender_host.mjs';

// The sender's log line when it finds the Mini's key in its list while it waits.
const SENDER_APPROVED = /approved the KVM ((?:[0-9A-F]{2}:){7}[0-9A-F]{2})/;
// The Mini's error for a clipboard call without a session.
const NO_CLIPBOARD = { code: -32603, data: 'no OS Services clipboard is connected' };
// Each time is taken this many times.
const ROUNDS = 3;
const PROBE = new URL('../../e2e/os_services_usb_probe.py', import.meta.url);

// Prints "yes" when the sender's running program has this version. On Linux
// the version is text in the program file. On macOS the test reads it from
// the app bundle's Info.plist, which gives a dev release's version without
// the suffix.
const HAS_VERSION = {
  linux: (pid, version) => `grep -qaF -- ${quote(version)} /proc/${pid}/exe && echo yes || true`,
  macos: (pid, version) => `exe=$(ps -o comm= -p ${pid}); ` +
    `[ "$(plutil -extract CFBundleShortVersionString raw "\${exe%/MacOS/*}/Info.plist")" = ${quote(version.replace(/-.*/, ''))} ] && echo yes || true`,
};

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// The session between the Mini and the OS Services sender on the bench's
// host: the USB interface that carries it, the sender's approval of the
// Mini, and its end and start with the sender. The tests read the host's USB
// devices, so they need a Linux or macOS host.
describe('the OS Services session', () => {
  let mini;
  let host;
  let session;
  let skip;
  // A host that the tests are not for skips each of them.
  const step = (name, options, run) => test(name, options, t => (skip ? t.skip(skip) : run(t)));

  before(async () => {
    mini = await connect();
    try {
      host = await senderHost(mini);
      if (host.system === 'windows') skip = `${host.login} runs Windows, and the tests read the host's USB devices as a Linux or macOS host shows them`;
      else session = await openSession(mini, host);
    } catch (error) {
      await mini.close().catch(() => undefined);
      throw error;
    }
  }, { timeout: 300_000 });

  after(async () => {
    try {
      await session?.restore();
    } finally {
      await mini.close();
    }
  }, { timeout: 300_000 });

  // Asks the Mini for its state every 20 ms until check(state) is true.
  // Returns that state and the time since `from`; throws after `seconds`.
  const reached = async (from, check, seconds) => {
    for (let state; ; await sleep(20)) {
      state = await videoState(mini).catch(() => state);
      if (state && check(state)) return { state, ms: Date.now() - from };
      if (Date.now() - from > seconds * 1000) throw new Error(`not reached after ${seconds} s; last: ${JSON.stringify(state)}`);
    }
  };
  const clipboardError = async (method, params) => {
    const { error } = await rpcAnswer(mini, method, params);
    return { code: error?.code, data: error?.data };
  };
  const interfaceDriver = async () => (await host.usb(mini)).osServices?.driver;
  // A text from the Mini to the clipboard of the host's desktop, and one
  // back. On a host whose clipboard cannot be read, a text to the sender and back.
  const clipboardRoundTrip = async label => {
    const desktop = await hostClipboard(host);
    const out = `${label}: to the host ${Date.now()} ✓`;
    assert.deepEqual(await mini.rpc('guestClipboardSet', { text: out }, 15_000), { status: 'ok' });
    if (desktop) assert.equal(await desktop.paste(), out, `${label}: the host pastes the Mini's text`);
    const back = desktop ? `${label}: from the host ${Date.now()} ✓` : out;
    await desktop?.copy(back);
    assert.deepEqual(await mini.rpc('guestClipboardGet', {}, 15_000), { status: 'ok', text: back, truncated: false }, `${label}: the Mini reads the host's text`);
  };
  // A videoInputState notification since `since` that passes check, within 10 s.
  const notified = (since, check) => until(() => mini.timedEvents('videoInputState'),
    events => events.some(event => event.at >= since && event.params.osServices !== undefined && check(event.params)), 10);

  step('the host lists one OS Services USB interface on the Mini, with a bulk IN and a bulk OUT endpoint', { timeout: 60_000 }, async () => {
    const { device, osServices } = await host.usb(mini);
    assert.ok(osServices, `${device.id} has the interface ff/4a/01: ${JSON.stringify(device.interfaces)}`);
    assert.equal(osServices.endpointCount, 2, 'bNumEndpoints');
    assert.deepEqual(osServices.endpoints.map(endpoint => endpoint.type), ['Bulk', 'Bulk']);
    assert.deepEqual(osServices.endpoints.map(endpoint => endpoint.direction).sort(), ['in', 'out']);
    for (const endpoint of osServices.endpoints) {
      assert.equal((Number.parseInt(endpoint.address, 16) & 0x80) !== 0, endpoint.direction === 'in',
        `endpoint 0x${endpoint.address} is ${endpoint.direction}`);
    }
    const packet = device.speed === '480' ? 512 : 64;
    assert.deepEqual(osServices.endpoints.map(endpoint => endpoint.maxPacketSize), [packet, packet], `wMaxPacketSize at ${device.speed} Mbit/s`);
    assert.ok(device.interfaces.some(one => one.triple.startsWith('03') && one.driver === 'usbhid'), 'HID interfaces beside it, bound to usbhid');
    // The sender of the open session claims the interface through usbfs.
    assert.equal(osServices.driver, 'usbfs', 'the sender holds the interface');
    assert.equal(device.version, '2.10', 'bcdUSB 2.10, for the BOS descriptor');
  });

  step('an unapproved Mini waits for approval, and connects within 10 s of it', { timeout: 600_000 }, async t => {
    const { fingerprint } = session;
    assert.match(fingerprint, FINGERPRINT, 'the Mini\'s key');
    const listed = async () => (await host.approvals()).content.split('\n').map(line => line.replace(/[-: ]/g, '').toUpperCase()).filter(Boolean).sort();
    const key = fingerprint.replace(/:/g, '');
    const found = await listed();
    const times = [];
    // A round that fails after the sender forgot the Mini leaves it approved,
    // as every round does at its end.
    let forgotten = false;
    t.after(() => (forgotten ? sender(mini, '--approve-kvm', fingerprint) : undefined));
    for (let round = 1; round <= ROUNDS; round++) {
      await sender(mini, '--forget-kvm', fingerprint);
      forgotten = true;
      assert.ok(!(await listed()).includes(key), 'the Mini is no longer in the sender\'s list');
      // The sender reads its list when a session opens, so it refuses the
      // Mini at its next start.
      const restartedAt = await host.now();
      await host.exec(`${host.commands.stopSender}; ${host.commands.startSender}`, { timeoutMs: 60_000 });
      const pending = await until(() => videoState(mini), state => state.osServices.approvalPending && !state.osServices.connected, 30);
      assert.equal(pending.osServices.sender, undefined, 'no sender details without a session');
      assert.equal(pending.guestClipboard, undefined, 'no clipboard without a session');
      assert.equal(pending.osServices.fingerprint, fingerprint, 'the key that the Mini reports');
      const refused = await host.waitForSenderLog(restartedAt, SENDER_NOT_APPROVED, 15);
      assert.equal(SENDER_NOT_APPROVED.exec(refused.message)[1], fingerprint, 'the sender refused the key that the Mini reports');
      assert.deepEqual(await clipboardError('guestClipboardGet'), NO_CLIPBOARD, 'no clipboard while the Mini waits');
      // Nothing but the approval lets the Mini in.
      await sleep(3000);
      const still = await videoState(mini);
      assert.equal(still.osServices.connected, false, 'not connected without approval');
      assert.equal(still.osServices.approvalPending, true, 'still waiting for approval');

      // A sender that waits reads its list once per second. The time counts
      // from the start of the command that approves.
      const approvedAt = Date.now();
      await sender(mini, '--approve-kvm', fingerprint);
      forgotten = false;
      const { ms } = await reached(approvedAt, connected, 15);
      times.push(ms);
      const state = await until(() => videoState(mini), connectedWithClipboard, 30);
      const approved = await host.waitForSenderLog(restartedAt, SENDER_APPROVED, 15);
      assert.equal(SENDER_APPROVED.exec(approved.message)[1], fingerprint, 'the sender approved the key that the Mini reports');
      await host.waitForSenderLog(approved.at, SENDER_SESSION_READY, 15);
      const { version } = state.osServices.sender;
      const { pid } = await host.sender();
      assert.equal((await host.exec(HAS_VERSION[host.system](pid, version))).trim(), 'yes',
        `the sender's program (process ${pid}) has the version ${version} that the Mini reports`);
      assert.ok(Array.isArray(state.osServices.sender.displays), 'the sender reports its displays');
      assert.equal(state.osServices.fingerprint, fingerprint);
      assert.equal(await interfaceDriver(), 'usbfs', 'the sender holds the interface');
      await notified(approvedAt, connected);
    }
    t.diagnostic(`from the approval to the session: ${times.join(', ')} ms`);
    for (const ms of times) assert.ok(ms < 10_000, `the Mini connected ${ms} ms after the approval`);
    assert.deepEqual(await listed(), found, 'the sender\'s list of approved KVMs is as found');
  });

  step('stopping the sender ends the session and frees the interface, and starting it connects again', { timeout: 600_000 }, async t => {
    const { fingerprint } = session;
    const closed = [];
    const reconnected = [];
    const before = await host.usb(mini);

    // A failure between the stop and the start leaves the sender running, as it was.
    let senderOff = false;
    t.after(() => (senderOff ? host.exec(host.commands.startSender, { timeoutMs: 60_000 }) : undefined));
    // Round 1: the stop and the start apart, with the state between them.
    let from = Date.now();
    senderOff = true;
    let stopping = host.exec(host.commands.stopSender, { timeoutMs: 60_000 });
    const down = await reached(from, state => !state.osServices.connected, 15);
    await stopping;
    closed.push(down.ms);
    assert.equal(down.state.osServices.sender, undefined, 'no sender details');
    assert.equal(down.state.guestClipboard, undefined, 'no clipboard');
    assert.equal(down.state.osServices.approvalPending, false, 'a stop is not a refusal');
    assert.equal(down.state.osServices.fingerprint, fingerprint, 'the key does not depend on a session');
    assert.deepEqual(await clipboardError('guestClipboardGet'), NO_CLIPBOARD);
    assert.deepEqual(await clipboardError('guestClipboardSet', { text: 'nobody listens' }), NO_CLIPBOARD);
    const stopped = await host.usb(mini);
    assert.equal(stopped.osServices?.driver, null, 'the interface is released');
    assert.equal(stopped.device.devnum, before.device.devnum, 'the Mini stays enumerated');
    const startedAt = await host.now();
    from = Date.now();
    await host.exec(host.commands.startSender, { timeoutMs: 60_000 });
    senderOff = false;
    reconnected.push((await reached(from, connectedWithClipboard, 30)).ms);
    await host.waitForSenderLog(startedAt, SENDER_SESSION_READY, 15);
    assert.equal(await interfaceDriver(), 'usbfs', 'the sender holds the interface again');
    await clipboardRoundTrip('after the sender started');

    // The other rounds: a restart, with both times from its start.
    for (let round = 2; round <= ROUNDS; round++) {
      from = Date.now();
      stopping = host.exec(`${host.commands.stopSender}; ${host.commands.startSender}`, { timeoutMs: 60_000 });
      closed.push((await reached(from, state => !state.osServices.connected, 15)).ms);
      reconnected.push((await reached(from, connectedWithClipboard, 30)).ms);
      await stopping;
    }
    t.diagnostic(`from the stop to the session's end: ${closed.join(', ')} ms; to the next session: ${reconnected.join(', ')} ms ` +
      '(round 1 from the start, the others from the stop)');
    for (const ms of closed) assert.ok(ms < 10_000, `the session ended ${ms} ms after the stop`);
    for (const ms of reconnected) assert.ok(ms < 20_000, `the Mini connected again after ${ms} ms`);
  });

  step('the raw USB transport answers RESET and STATUS as the OS Services protocol requires, and stalls every other request', { timeout: 300_000 }, async t => {
    if (!(await host.exec('command -v python3 >/dev/null && echo yes || true')).trim()) {
      return t.skip(`${host.login} has no python3 for tools/e2e/os_services_usb_probe.py`);
    }
    const { device, osServices } = await host.usb(mini);
    assert.ok(osServices, 'the Mini has the OS Services interface');
    // The probe needs the interface, which a running sender holds. macOS
    // lets the login's user claim an interface that no driver holds.
    await host.exec(host.commands.stopSender, { timeoutMs: 60_000 });
    let report;
    try {
      await until(() => videoState(mini), state => !state.osServices.connected, 15);
      const sudo = host.system === 'macos' ? '' : 'sudo -n ';
      // The report is the last line; the lines before it are the probe's log.
      const lines = (await host.exec(`${sudo}python3 - --json --bus ${device.busnum} --address ${device.devnum} 2>&1; true`,
        { input: fs.readFileSync(PROBE), timeoutMs: 90_000 })).trim().split('\n');
      const log = lines.slice(0, -1).join('\n');
      try {
        report = JSON.parse(lines.at(-1));
      } catch {
        throw new Error(`the probe printed no report:\n${lines.join('\n')}`);
      }
      if (report.unavailable?.includes('libusb-1.0 not found')) return t.skip(`${host.login} has no libusb-1.0`);
      assert.equal(report.unavailable, undefined, 'the probe opened the interface');
      assert.deepEqual(report.checks.filter(check => !check.ok), [], `failed checks of the probe; its log:\n${log}`);
      assert.equal(report.ok, true);
      const found = { bus: report.device.bus, address: report.device.address, id: report.device.id, interface: report.device.interface };
      assert.deepEqual(found, { bus: device.busnum, address: device.devnum, id: device.id, interface: osServices.number }, 'the probe found the Mini\'s interface');
      const tokens = report.resets.map(reset => reset.token);
      assert.equal(tokens.length, 3, 'three RESETs');
      assert.equal(new Set(tokens).size, 3, 'each RESET makes a new token');
      assert.ok(report.checks.some(check => check.name === 'unknown bRequest 0x42 stalls'), 'the probe checked the stalls');
      const ready = report.resets.map(reset => reset.ready_ms);
      t.diagnostic(`from RESET to ready: ${ready.join(', ')} ms; one control transfer: ${report.control_rtt_ms.join(', ')} ms`);
      for (const ms of ready) assert.ok(typeof ms === 'number' && ms < 15_000, `ready ${ms} ms after RESET`);
      // A RESET readies the transport and is no session.
      const state = await videoState(mini);
      assert.equal(state.osServices.connected, false, 'a RESET alone is not a session');
      assert.equal(state.osServices.sender, undefined);
    } finally {
      await host.exec(host.commands.startSender, { timeoutMs: 60_000 });
    }
    // The sender's own RESET replaces what the probe left.
    await until(() => videoState(mini), connectedWithClipboard, 45);
  });
});
