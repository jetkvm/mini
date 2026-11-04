import assert from 'node:assert/strict';
import http from 'node:http';
import https from 'node:https';
import { after, before, describe, test } from 'node:test';
import { serverCertificate } from '../../combined_load/runner/fake_cloud.mjs';
import {
  FINGERPRINT, SENDER_DEVICE_GONE, SENDER_NOT_APPROVED, SENDER_PROTOCOL_ERROR, SENDER_SESSION_READY, senderHost, usbIdentity,
} from '../lib/sender_host.mjs';
import { approveMini } from '../lib/approval.mjs';
import { connect, mockCloudBuild, until } from '../lib/mini.mjs';
import { connected, connectedWithClipboard, hook, openSession, rpcAnswer, videoState } from '../lib/os_services.mjs';
import { updateServer } from '../lib/servers.mjs';

// The version under which the stand-in update server offers the image (lib/servers.mjs).
const OFFERED = '99.0.0';
// The update server that the Mini uses outside a test, and its release from
// before OS Services.
const PRODUCTION = 'https://api.jetkvm.com';
const PREVIOUS_VERSION = '0.5.10';
// How long before the Mini is seen down for the restart the session may end.
const RESTART_WINDOW_MS = 20_000;
// A Mini that has restarted into a new image confirms it 10 seconds after
// its start; a restart or another install before that would roll it back.
const CONFIRM_WAIT_MS = 15_000;
// macOS ignores a Caps Lock press shorter than its Caps Lock delay; 200 ms
// toggles it. Linux toggles on the press, whatever its length.
const CAPS_LOCK = 0x39;
const CAPS_LOCK_HOLD_MS = 200;
const CLIPBOARD_OFF = 'clipboard sharing is turned off in the OS Services settings';
const METHOD_NOT_FOUND = -32601;

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const messages = entries => entries.map(entry => entry.message);
// The request that stores `settings` again, an empty adaptation included: the Mini leaves that out when it reads.
const settingsRequest = settings => ({ ...settings, adaptation: settings.adaptation ?? '' });

// Firmware updates with an OS Services session open between the Mini and the
// sender on the bench's host. An install here offers the image that the Mini
// already runs, as a higher version. The tests read the host's USB port and
// the sender's log, so they need a Linux or macOS host.
describe('OS Services across firmware updates', () => {
  let mini;
  let host;
  let session;
  let skip;
  // A host that the tests are not for skips each of them.
  const step = (name, options, run) => test(name, options, t => (skip ? t.skip(skip) : run(t)));
  let updateUrl;
  let port;

  const up = () => fetch(new URL('/device/status', mini.url), { signal: AbortSignal.timeout(1000) }).then(response => response.ok, () => false);
  // Down after three missed probes, ~1.25 s each, which fit in the few
  // seconds an Ethernet Mini is away for its restart.
  const waitDown = async seconds => {
    const end = Date.now() + seconds * 1000;
    for (let misses = 0; misses < 3;) {
      if (Date.now() > end) throw new Error(`the Mini did not go down for its restart in ${seconds} s`);
      misses = (await up()) ? 0 : misses + 1;
      await sleep(250);
    }
  };

  // Asks for the Mini's state every intervalMs until stop() and keeps one
  // sample per change of the session, the approval and the clipboard:
  // { sent, answered, connected, approvalPending, clipboard }.
  const watchSession = intervalMs => {
    const samples = [];
    let stopped = false;
    const done = (async () => {
      for (let last = ''; !stopped; await sleep(intervalMs)) {
        const sent = Date.now();
        const { result } = await rpcAnswer(mini, 'getVideoState', {}, 5000).catch(() => ({}));
        if (!result?.osServices) continue;
        const sample = {
          sent, answered: Date.now(), connected: result.osServices.connected === true,
          approvalPending: result.osServices.approvalPending === true, clipboard: result.guestClipboard !== undefined,
        };
        const key = `${sample.connected}/${sample.approvalPending}/${sample.clipboard}`;
        if (key !== last) samples.push(sample);
        last = key;
      }
    })();
    return { samples, stop: () => { stopped = true; return done; } };
  };

  // Installs a version from the update server at base, and returns when the
  // Mini answers its RPC with that version, its state passes check (when
  // given) and it has confirmed the image. installed() fails unless the Mini
  // runs what was asked for.
  const installFrom = async (base, version, installed, check) => {
    try {
      await mini.rpc('setUpdateApiUrl', { url: base });
      const status = await mini.rpc('checkUpdateComponents', { params: { components: { system: version } } }, 30_000);
      assert.equal(status.systemUpdateAvailable, true, `the Mini takes ${version}`);
      const watch = watchSession(100);
      let hostTrigger;
      let trigger;
      let down;
      try {
        hostTrigger = await host.now();
        trigger = Date.now();
        // The Mini can restart before its answer arrives.
        mini.rpc('tryUpdateComponents', { params: { components: { system: version } }, includePreRelease: false, resetConfig: false }, 10_000)
          .catch(() => undefined);
        await waitDown(300);
        down = Date.now();
      } finally {
        // Also when the Mini does not go down: a watch that goes on keeps
        // the test's process alive.
        await watch.stop();
      }
      await sleep(2000);
      await until(up, Boolean, 180);
      const upAt = Date.now();
      await mini.open();
      let reached;
      if (check) reached = { state: await until(() => videoState(mini), check, 120), at: Date.now() };
      await installed();
      await sleep(Math.max(0, upAt + CONFIRM_WAIT_MS - Date.now()));
      return { hostTrigger, trigger, down, samples: watch.samples, reached };
    } finally {
      await mini.rpc('setUpdateApiUrl', { url: updateUrl }).catch(() => undefined);
    }
  };
  const runsImage = async () => assert.equal((await mini.status()).firmware.elfSha256, mini.image.elfSha256, 'the Mini runs the image');
  // The image that the Mini runs, as a higher version.
  const installImage = async check => {
    const server = await updateServer(mini);
    try {
      return await installFrom(server.base, OFFERED, runsImage, check);
    } finally {
      server.close();
    }
  };

  // Proxies updateServer()'s check answer through change(release) and serves
  // the image itself; records queries and imageRequests. `secure` uses HTTPS.
  const changedUpdateServer = async (change, { secure = false } = {}) => {
    const upstream = await updateServer(mini);
    const address = new URL(upstream.base).hostname;
    const queries = [];
    const seen = { imageRequests: 0 };
    const answer = async (request, response) => {
      const { pathname, search, searchParams } = new URL(request.url, 'http://localhost');
      if (pathname === '/jetkvm-mini.bin') {
        seen.imageRequests++;
        return void response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': mini.image.bytes.length })
          .end(mini.image.bytes);
      }
      if (pathname !== '/releases') return void response.writeHead(404).end();
      queries.push(Object.fromEntries(searchParams));
      const release = await (await fetch(`${upstream.base}/releases${search}`)).json();
      release.systemUrl = `${base()}/jetkvm-mini.bin`;
      response.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify(change(release)));
    };
    const server = secure ? https.createServer(serverCertificate(address), answer) : http.createServer(answer);
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, resolve);
    });
    const base = () => `${secure ? 'https' : 'http'}://${address}:${server.address().port}`;
    return {
      base: base(), queries, seen,
      close: () => {
        server.closeAllConnections();
        server.close();
        upstream.close();
      },
    };
  };
  // The image under a SHA-256 that it does not have: the Mini downloads it
  // and then refuses it.
  const wrongHash = release => ({ ...release, systemHash: release.systemHash.replace(/^./, digit => (digit === '0' ? '1' : '0')) });
  const requestImage = () => void mini.rpc('tryUpdateComponents',
    { params: { components: { system: OFFERED } }, includePreRelease: false, resetConfig: false }, 10_000).catch(() => undefined);

  // Restarts the watch on the Mini's USB port of the host.
  const watchPort = async name => {
    port?.stop();
    port = await host.watchPort(name);
    return port;
  };

  // The text goes to the sender and back.
  const clipboardRoundTrip = async label => {
    const text = `${label} ${Date.now()} ✓`;
    assert.deepEqual(await mini.rpc('guestClipboardSet', { text }, 15_000), { status: 'ok' });
    assert.deepEqual(await mini.rpc('guestClipboardGet', {}, 15_000), { status: 'ok', text, truncated: false });
  };

  // Caps Lock out and its LED back through the host's HID driver, twice, so
  // that the LED ends as it was.
  const capsLockRoundTrip = async () => {
    const led = () => hook(mini, 'getKeyboardLedState');
    const press = down => hook(mini, 'sendKeypress', CAPS_LOCK, down);
    const initial = await led();
    assert.notEqual(initial, null, 'the keyboard\'s LED state');
    for (const expected of [!initial.caps_lock, initial.caps_lock]) {
      await press(true);
      await sleep(CAPS_LOCK_HOLD_MS);
      await press(false);
      await until(led, state => state?.caps_lock === expected, 5);
    }
  };

  const cleanup = async () => {
    const errors = [];
    const step = run => run().catch(error => errors.push(error));
    port?.stop();
    port = undefined;
    if (session) {
      await step(async () => {
        await mini.open();
        // A test that stopped half way can leave the release without OS Services.
        if ((await rpcAnswer(mini, 'getOsServicesSettings')).error?.code === METHOD_NOT_FOUND) await installImage();
        await mini.rpc('setUpdateApiUrl', { url: updateUrl });
      });
      await step(() => session.restore());
      session = undefined;
    }
    if (mini) await step(() => mini.close());
    mini = undefined;
    if (errors.length) throw new AggregateError(errors, `not as found: ${errors.map(error => error.message).join('; ')}`);
  };

  before(async () => {
    try {
      mini = await connect();
      host = await senderHost(mini);
      if (host.system === 'windows') {
        skip = `${host.login} runs Windows, and the tests read the host's USB port as a Linux or macOS host shows it`;
        await mini.close();
        mini = undefined;
        return;
      }
      updateUrl = await mini.rpc('getUpdateApiUrl');
      session = await openSession(mini, host);
    } catch (error) {
      await cleanup().catch(() => undefined);
      throw error;
    }
  }, { timeout: 300_000 });

  after(() => cleanup(), { timeout: 900_000 });

  step('an install ends the session at the restart, and the sender reconnects on its own with the same key, settings and USB identity',
    { timeout: 900_000 }, async t => {
      // A check that does not end the test: every one is reported at the end.
      const soft = [];
      await runsImage();
      const first = await until(() => videoState(mini), connectedWithClipboard, 30);

      // Settings to read back after the install.
      const settings = await mini.rpc('getOsServicesSettings');
      const changed = {
        clipboard_enabled: false,
        resolution: settings.resolution === '1920x1080' ? '1280x720' : '1920x1080',
        terminal_enabled: !settings.terminal_enabled,
      };
      const applied = await mini.rpc('setOsServicesSettings', { settings: changed });
      assert.deepEqual(applied, { ...settings, ...changed }, 'the changed settings');
      await until(() => videoState(mini), state => connected(state) && state.guestClipboard === undefined, 15);
      const usb = await host.usb(mini);
      assert.equal(usb.osServices?.driver, 'usbfs', 'the sender holds the interface');
      const { pid } = await host.sender();
      const before = {
        fingerprint: first.osServices.fingerprint, approvals: await host.approvals(),
        identity: usbIdentity(usb.device), lsusb: await host.lsusb(usb.device.id),
      };
      await watchPort(usb.device.name);

      const installed = await installImage(connected);
      await until(() => videoState(mini), state => Boolean(state.osServices.sender), 10);

      // Up to the restart the session was open: the download and the flash
      // writes do not end it. The Mini counts as down after three missed
      // probes, up to 4 s after it went; a session that ends with the
      // restart ends within that window.
      const early = installed.samples.filter(sample => !sample.connected && sample.answered < installed.down - RESTART_WINDOW_MS);
      if (early.length) soft.push(`the session ended before the restart: ${JSON.stringify(installed.samples)}`);

      // The host: the Mini left the bus once, came back and was claimed again.
      const times = port.reenumeration(installed.hostTrigger);
      const samples = JSON.stringify(port.samples);
      assert.notEqual(times.goneAt, undefined, `the Mini left the host's bus: ${samples}`);
      assert.notEqual(times.backAt, undefined, `the host enumerated it again: ${samples}`);
      assert.notEqual(times.claimedAt, undefined, `the sender claimed the interface again: ${samples}`);
      if (times.departures !== 1) soft.push(`one restart, and ${times.departures} departures: ${samples}`);

      // The sender: the device went away, no protocol error, a new session.
      const entries = await host.senderLog(installed.hostTrigger);
      const log = messages(entries).join('\n');
      assert.deepEqual(messages(entries.filter(entry => SENDER_PROTOCOL_ERROR.test(entry.message))), [],
        'protocol errors in the sender\'s log');
      assert.ok(entries.some(entry => entry.at >= times.goneAt - 1000 && SENDER_DEVICE_GONE.test(entry.message)),
        `the sender saw the Mini go; its log:\n${log}`);
      const ready = entries.find(entry => entry.at >= times.backAt && SENDER_SESSION_READY.test(entry.message));
      assert.ok(ready, `a new session after the restart; the sender's log:\n${log}`);
      assert.deepEqual(messages(entries.filter(entry => SENDER_NOT_APPROVED.test(entry.message))), [], 'no approval asked');
      const service = await host.sender();
      assert.equal(service.running, true, 'the sender is running');
      assert.equal(service.pid, pid, 'the same sender process reconnected: no crash, no restart');

      for (const [name, ms] of Object.entries({
        'the request until the Mini leaves the host\'s bus (host clock)': times.goneAt - installed.hostTrigger,
        'off the bus until the host enumerates it again (host clock)': times.backAt - times.goneAt,
        'enumerated until the sender claims the interface (host clock)': times.claimedAt - times.backAt,
        'enumerated until the sender logs its new session (host clock)': ready.at - times.backAt,
        'the request until the sender logs its new session (host clock)': ready.at - installed.hostTrigger,
        'the request until the reloaded page reads the session as open (this machine\'s clock)': installed.reached.at - installed.trigger,
      })) t.diagnostic(`${name}: ${Math.round(ms)} ms`);
      assert.ok(ready.at - times.backAt < 30_000, `the sender reconnects within 30 s of enumeration: ${Math.round(ready.at - times.backAt)} ms`);

      // The Mini keeps its key and its approval.
      const state = await until(() => videoState(mini), connected, 60);
      assert.equal(state.osServices.fingerprint, before.fingerprint, 'the key survived the update');
      assert.match(state.osServices.fingerprint, FINGERPRINT);
      assert.equal(state.osServices.approvalPending, false, 'no approval pending');
      assert.deepEqual(await host.approvals(), before.approvals, 'the sender connected without a change of its list');

      // The settings and the USB identity survive.
      assert.deepEqual(await mini.rpc('getOsServicesSettings'), applied, 'the settings changed before the update');
      assert.equal((await videoState(mini)).guestClipboard, undefined, 'sharing is still off');
      const refusal = (await rpcAnswer(mini, 'guestClipboardGet')).error;
      assert.deepEqual({ code: refusal?.code, data: refusal?.data }, { code: -32603, data: CLIPBOARD_OFF });
      const { device } = await host.usb(mini);
      assert.deepEqual(usbIdentity(device), before.identity, 'the USB identity that the host sees');
      if (before.lsusb) {
        const now = await host.lsusb(device.id);
        assert.equal(now?.line, before.lsusb.line, 'lsusb');
        if (now?.verbose !== before.lsusb.verbose) soft.push(`lsusb -v changed:\n${before.lsusb.verbose}\nnow:\n${now?.verbose}`);
      }

      await mini.rpc('setOsServicesSettings', { settings: settingsRequest(settings) });
      assert.deepEqual(await mini.rpc('getOsServicesSettings'), settings);
      await until(() => videoState(mini), connectedWithClipboard, 30);
      assert.deepEqual(soft, []);
    });

  step('a refused update leaves the session alone', { timeout: 900_000 }, async t => {
    const soft = [];
    await runsImage();
    const running = (await mini.status()).firmware.version;
    await until(() => videoState(mini), connectedWithClipboard, 30);
    const { device } = await host.usb(mini);
    const { pid } = await host.sender();
    await watchPort(device.name);
    const hostStart = await host.now();
    const watch = watchSession(100);
    try {
      // A check on the stable channel that finds the release that the Mini
      // runs: nothing to install.
      let server = await changedUpdateServer(release => ({ ...release, systemVersion: running }));
      try {
        await mini.rpc('setUpdateApiUrl', { url: server.base });
        await mini.rpc('setDevChannelState', { enabled: false });
        const status = await mini.rpc('getUpdateStatus', {}, 30_000);
        assert.ok(!status.systemUpdateAvailable, 'no update offered');
        assert.equal(server.queries.at(-1)?.prerelease, 'false', 'the Mini asked for the stable channel');
      } finally {
        await mini.rpc('setUpdateApiUrl', { url: updateUrl });
        server.close();
      }

      // An image whose hash does not match: downloaded, then refused.
      server = await changedUpdateServer(wrongHash);
      try {
        await mini.rpc('setUpdateApiUrl', { url: server.base });
        const events = (await mini.events('otaState')).length;
        requestImage();
        await until(() => server.seen.imageRequests, Boolean, 60);
        await until(() => mini.rpc('isUpdatePending', {}, 10_000), pending => pending === false, 300);
        // Nothing queued behind it, and no restart after it.
        await sleep(5000);
        assert.equal(await mini.rpc('isUpdatePending'), false, 'no install pending');
        const errors = (await mini.events('otaState')).slice(events).map(state => state.error).filter(Boolean);
        if (!errors.some(error => error.startsWith('Error verifying system update: hash mismatch: '))) soft.push(`the refusal that the update dialog shows: ${JSON.stringify(errors)}`);
      } finally {
        await mini.rpc('setUpdateApiUrl', { url: updateUrl });
        server.close();
      }
    } finally {
      await watch.stop();
    }

    await runsImage();
    assert.ok(watch.samples.length > 0 && watch.samples.every(sample => sample.connected && sample.clipboard),
      `open with the clipboard throughout: ${JSON.stringify(watch.samples)}`);
    const states = [port.stateAt(hostStart), ...port.samples.filter(sample => sample.at > hostStart)];
    assert.ok(states.every(sample => sample?.present && sample.osServices === 'usbfs'),
      `the Mini stayed on the bus with the sender holding it: ${JSON.stringify(states)}`);
    assert.equal(port.reenumeration(hostStart).departures, 0, 'no re-enumeration');
    const entries = await host.senderLog(hostStart);
    assert.deepEqual(messages(entries.filter(entry => SENDER_SESSION_READY.test(entry.message) ||
      SENDER_DEVICE_GONE.test(entry.message) || SENDER_PROTOCOL_ERROR.test(entry.message))), [],
    'no session ended or opened in the sender\'s log');
    assert.equal((await host.sender()).pid, pid, 'the same sender process');
    await clipboardRoundTrip('after the refused updates');
    assert.deepEqual(soft, []);
  });

  step('from a release without OS Services and back', { timeout: 1_800_000 }, async t => {
    const soft = [];
    await runsImage();
    const first = await until(() => videoState(mini), connected, 30);
    const keyBefore = first.osServices.fingerprint;
    const usbBefore = await host.usb(mini);

    // Down to the release, without OS Services, from the production update server.
    await watchPort(usbBefore.device.name);
    let installed = await installFrom(PRODUCTION, PREVIOUS_VERSION, async () => assert.equal(
      (await mini.rpc('getLocalVersion', {}, 10_000)).systemVersion, PREVIOUS_VERSION, 'the installed version'));
    assert.equal((await rpcAnswer(mini, 'getOsServicesSettings')).error?.code, METHOD_NOT_FOUND, `${PREVIOUS_VERSION} has no OS Services`);
    const old = await host.usb(mini);
    assert.equal(old.osServices, undefined, 'no OS Services interface on the host');
    const hid = old.device.interfaces.filter(one => one.triple.startsWith('03'));
    assert.ok(hid.length > 0, 'HID interfaces');
    assert.deepEqual(hid.map(one => one.driver), hid.map(() => 'usbhid'), 'every HID interface bound to usbhid');
    await capsLockRoundTrip();
    let times = port.reenumeration(installed.hostTrigger);
    assert.notEqual(times.backAt, undefined, `the host enumerated it again: ${JSON.stringify(port.samples)}`);
    assert.ok(port.samples.filter(sample => sample.at >= times.backAt).every(sample => sample.osServices === 'none'),
      'nothing left of the interface');
    // The sender keeps looking without holding anything, and keeps running.
    await host.waitForSenderLog(times.backAt, /no OS Services USB interface found/, 30);
    assert.equal((await host.sender()).running, true, 'the sender keeps running');
    t.diagnostic(`into ${PREVIOUS_VERSION}: off the bus until enumerated again: ${times.backAt - times.goneAt} ms (host clock)`);

    // Up to the image again.
    await watchPort(usbBefore.device.name);
    installed = await installImage(state => state.osServices?.connected === true || state.osServices?.approvalPending === true);
    const key = installed.reached.state.osServices.fingerprint;
    assert.match(key, FINGERPRINT, 'a key');
    const now = await host.usb(mini);
    assert.notEqual(now.osServices, undefined, 'the interface is back');
    assert.deepEqual(now.osServices.endpoints.map(e => `${e.type} ${e.direction}`).sort(), ['Bulk in', 'Bulk out']);
    if (key !== keyBefore) {
      soft.push(`the key ${keyBefore} from before, which the release without OS Services keeps, is ${key} now`);
      // A new key: it is approved and stays approved, so the bench still works.
      await approveMini(mini);
    }
    await until(() => videoState(mini), connected, 60);
    times = port.reenumeration(installed.hostTrigger);
    assert.notEqual(times.backAt, undefined, `the host enumerated it again: ${JSON.stringify(port.samples)}`);
    let ready = await host.waitForSenderLog(times.backAt, SENDER_SESSION_READY, 60);
    t.diagnostic(`into the image: off the bus until enumerated again: ${times.backAt - times.goneAt} ms; ` +
      `enumerated until the sender logs its new session: ${Math.round(ready.at - times.backAt)} ms; ` +
      `the request until that session: ${Math.round(ready.at - installed.hostTrigger)} ms (host clock)`);

    // A second restart keeps the key.
    await watchPort(usbBefore.device.name);
    const restarted = await host.now();
    mini.rpc('reboot', { force: true }, 5000).catch(() => undefined);
    await waitDown(30);
    await until(up, Boolean, 120);
    await mini.open();
    const state = await until(() => videoState(mini), connected, 90);
    assert.equal(state.osServices.fingerprint, key, 'the same key after a restart');
    assert.equal(state.osServices.approvalPending, false, 'no approval asked');
    times = port.reenumeration(restarted);
    assert.notEqual(times.backAt, undefined, `the host enumerated it again: ${JSON.stringify(port.samples)}`);
    ready = await host.waitForSenderLog(times.backAt, SENDER_SESSION_READY, 60);
    t.diagnostic(`a restart of the image: off the bus until enumerated again: ${times.backAt - times.goneAt} ms; ` +
      `enumerated until the sender logs its new session: ${Math.round(ready.at - times.backAt)} ms (host clock)`);
    assert.deepEqual(soft, []);
  });

  step('service video pauses while an install writes the image and resumes after a refusal', { timeout: 600_000 }, async t => {
    await runsImage();
    // The image comes over HTTPS, from a server with a certificate of the test CA.
    const { version: firmware } = (await mini.status()).firmware;
    if (!mockCloudBuild(firmware)) return t.skip(`the Mini runs ${firmware}, which is not a mock cloud build: set JETKVM_MOCK_CLOUD_JWKS_URL in the bench file`);
    const settings = await mini.rpc('getOsServicesSettings');
    const registered = await until(() => videoState(mini),
      state => state.osServices.videoRegistered || state.osServices.videoError !== undefined, 15).catch(() => videoState(mini));
    assert.equal(registered.osServices.videoRegistered, true, 'the sender offers service video');
    const point = async () => {
      const response = await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(10_000) });
      if (!response.ok) throw new Error(`/webrtc/stats: ${response.status}`);
      const stats = await response.json();
      return {
        at: Date.now(), pending: (await mini.rpc('isUpdatePending', {}, 10_000)) === true, sent: stats.accessUnitsSent,
        frames: stats.guestVideo.frames, discarded: stats.guestVideo.framesDiscarded,
        dmaMinimum: stats.internalDmaHeapMinimum, psramMinimum: stats.psramMinimum,
      };
    };
    const decoded = async () => await mini.frames() ?? 0;
    const server = await changedUpdateServer(wrongHash, { secure: true });
    try {
      await mini.rpc('setOsServicesSettings', { settings: { video_enabled: true, resolution: '1920x1080' } });
      const showing = await until(() => videoState(mini), state => state.osServices.videoError !== undefined ||
        (state.source === 'guest' && state.osServices.video && state.ready && state.width === 1920 && state.height === 1080), 60);
      assert.equal(showing.osServices.videoError, undefined, 'the Mini\'s service video error');
      await until(() => hook(mini, 'getVideoStreamDimensions'),
        size => size?.width === 1920 && size?.height === 1080, 30);
      await until(() => hook(mini, 'isVideoStreamActive'), Boolean, 30);
      const before = await point();
      await sleep(3000);
      const streaming = await point();
      assert.ok(streaming.sent > before.sent, 'service video reaches the viewer before the install');

      await mini.rpc('setUpdateApiUrl', { url: server.base });
      requestImage();
      // The pause starts before the download; one second covers an access
      // unit that is already on its way.
      await until(() => server.seen.imageRequests, Boolean, 60);
      await sleep(1000);
      const installing = [];
      for (const end = Date.now() + 300_000; ;) {
        const now = await point();
        if (!now.pending) break;
        assert.ok(Date.now() < end, 'the install ends within 300 s');
        installing.push(now);
        await sleep(1000);
      }
      assert.ok(installing.length > 3, `${installing.length} samples while the install ran`);
      const first = installing[0];
      const last = installing.at(-1);
      t.diagnostic(`service video during ${((last.at - first.at) / 1000).toFixed(0)} s of the install: ` +
        `${last.sent - first.sent} access units sent, ${last.frames - first.frames} frames taken, ` +
        `${last.discarded - first.discarded} discarded; DMA low water ${last.dmaMinimum} B, PSRAM low water ${last.psramMinimum} B`);
      assert.equal(last.sent - first.sent, 0, 'access units sent while the install ran');
      assert.equal(last.frames - first.frames, 0, 'service video frames taken while the install ran');
      assert.ok(last.dmaMinimum >= 64 * 1024, `internal DMA low-water mark: ${last.dmaMinimum}`);
      assert.ok(last.psramMinimum >= 4 * 1024 * 1024, `PSRAM low-water mark: ${last.psramMinimum}`);

      const refused = await point();
      const decodedBefore = await decoded();
      await until(point, now => now.sent > refused.sent && now.frames > refused.frames, 30);
      await until(decoded, frames => frames > decodedBefore, 30);
      await runsImage();
    } finally {
      await mini.rpc('setUpdateApiUrl', { url: updateUrl });
      server.close();
      await mini.rpc('setOsServicesSettings', { settings: settingsRequest(settings) });
    }
  });
});
