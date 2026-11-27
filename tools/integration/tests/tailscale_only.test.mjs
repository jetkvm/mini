import assert from 'node:assert/strict';
import net from 'node:net';
import { test } from 'node:test';
import { diagnostics, pageRpc } from '../../combined_load/runner/page.mjs';
import { tailnetPath } from '../../combined_load/runner/tailscale.mjs';
import { connect, until } from '../lib/mini.mjs';
import { KEY_FILE, cleanState, join, status, tailnetAddress, tunnelHttp } from '../lib/tailscale.mjs';

// "Web interface through Tailscale only": the web server answers on the
// Mini's Tailscale address only. Only a session that came through Tailscale
// may turn it on; any session, and the Mini's screen, may turn it off.
const NEEDS_TAILSCALE = /Open the web interface through Tailscale to turn this on/;
const TURN_OFF_FIRST = /Turn off "Web interface through Tailscale only" first/;

// What one request on a new connection to the Mini's local address gets: the
// HTTP status, null when the Mini closed the connection without an answer, or
// 'nothing' when it did neither in time. The Mini keeps an answered
// connection open, so the status line ends the wait.
const localAnswer = (url, timeoutMs = 5000) => new Promise(resolve => {
  const { hostname, port } = new URL(url);
  const socket = net.connect({ host: hostname, port: Number(port) || 80 });
  let received = '';
  let answered = false;
  const done = value => {
    if (answered) return;
    answered = true;
    socket.destroy();
    resolve(value);
  };
  const ended = () => done(received ? Number(received.split(' ')[1]) : null);
  socket.setTimeout(timeoutMs, () => done('nothing'));
  socket.on('connect', () => socket.write(`GET /device/status HTTP/1.1\r\nHost: ${hostname}\r\n\r\n`));
  socket.on('data', chunk => {
    received += chunk;
    if (received.includes('\r\n')) ended();
  });
  socket.on('error', ended);
  socket.on('close', ended);
});

test('a session from the local network reads the Tailscale-only setting and cannot turn it on', { timeout: 120_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  assert.ok((await mini.rpc('getDeviceCapabilities')).includes('tailscale_only'));
  assert.deepEqual(await mini.rpc('getTailscaleOnlyAccess'), { enabled: false, throughTailscale: false });
  await assert.rejects(mini.rpc('setTailscaleOnlyAccess', { enabled: true }), NEEDS_TAILSCALE);
  assert.deepEqual(await mini.rpc('getTailscaleOnlyAccess'), { enabled: false, throughTailscale: false });
  assert.equal(await localAnswer(mini.url), 200, 'the local address answers');
  // Turning it off is allowed from any session; here it changes nothing.
  assert.deepEqual(await mini.rpc('setTailscaleOnlyAccess', { enabled: false }), { enabled: false, throughTailscale: false });
  assert.equal(await localAnswer(mini.url), 200);
});

test('turned on through Tailscale, the Mini answers on its Tailscale address only, until it is turned off', { timeout: 600_000 }, async t => {
  const mini = await connect();
  let joinedHere = false;
  // The session through Tailscale, which alone can turn the setting on, and
  // which turns it off again whatever the test did.
  let tunnel = null;
  let address = null;
  const openTunnel = async () => {
    const page = await mini.page.context().browser().newPage();
    await until(() => page.goto(`http://${address}/`, { waitUntil: 'domcontentloaded', timeout: 15_000 }), () => true, 90);
    const rpc = pageRpc(page);
    await until(() => rpc('getTailscaleOnlyAccess'), () => true, 90);
    return { page, rpc };
  };
  t.after(async () => {
    try {
      if (address) {
        // The rig must answer on its local address again: try on new
        // sessions through Tailscale until the setting is off.
        await until(async () => {
          if (await localAnswer(mini.url) === 200) return true;
          await tunnel?.page.close().catch(() => undefined);
          tunnel = await openTunnel();
          await tunnel.rpc('setTailscaleOnlyAccess', { enabled: false });
          return await localAnswer(mini.url) === 200;
        }, Boolean, 240);
      }
      await tunnel?.page.close().catch(() => undefined);
      await mini.open();
      if (joinedHere) await cleanState(mini);
    } finally {
      await mini.close();
    }
  });

  // A tailnet that this machine is on too: the one of the key file, or the
  // one that the Mini is in already.
  let joined = await status(mini);
  if (KEY_FILE) {
    await cleanState(mini);
    joined = await join(mini);
    joinedHere = true;
  } else if (!(joined.running && joined.self?.online && tailnetAddress(joined) &&
               await tailnetPath(tailnetAddress(joined)) !== null)) {
    t.skip('the Mini is not on a tailnet of this machine, and JETKVM_TAILSCALE_AUTH_KEY_FILE is not set');
    return;
  }
  address = tailnetAddress(joined);
  assert.equal(await tunnelHttp(address, 30_000), 200, 'HTTP reaches the Mini through the tunnel');

  tunnel = await openTunnel();
  assert.deepEqual(await tunnel.rpc('getTailscaleOnlyAccess'), { enabled: false, throughTailscale: true });
  assert.deepEqual(await tunnel.rpc('setTailscaleOnlyAccess', { enabled: true }), { enabled: true, throughTailscale: true });

  // The local address accepts no request, on a new connection and again.
  for (let attempt = 0; attempt < 3; attempt++) {
    assert.equal(await localAnswer(mini.url), null, 'the Mini closes a connection to its local address without an answer');
  }
  assert.equal(await tunnelHttp(address, 30_000), 200, 'the Tailscale address still serves');
  assert.deepEqual(await tunnel.rpc('getTailscaleOnlyAccess'), { enabled: true, throughTailscale: true });
  // The session through Tailscale goes on: it was not one of the closed ones.
  const stats = await (await fetch(`http://${address}/webrtc/stats`, { signal: AbortSignal.timeout(10_000) })).json();
  assert.equal(stats.peerConnected, true);

  // While it is on, the Mini does not leave its tailnet.
  await assert.rejects(tunnel.rpc('setTailscaleConfig', { enabled: false }), TURN_OFF_FIRST);
  await assert.rejects(tunnel.rpc('setTailscaleConfig', { enabled: true, authKey: 'tskey-auth-integration-placeholder' }), TURN_OFF_FIRST);
  await assert.rejects(tunnel.rpc('setTailscaleControlURL', { controlURL: 'https://headscale.invalid:8443' }), TURN_OFF_FIRST);
  await assert.rejects(tunnel.rpc('logoutTailscale'), TURN_OFF_FIRST);
  assert.equal((await tunnel.rpc('getTailscaleStatus')).running, true);

  // The setting is stored: after a restart the local address stays closed
  // and the Tailscale address serves again.
  const restartAt = Date.now();
  tunnel.rpc('reboot', { force: true }, 5000).catch(() => undefined);
  // The Mini's own count of seconds since it started, read through the tunnel.
  await until(async () => (await diagnostics(`http://${address}`)).uptimeSeconds,
    seconds => seconds < (Date.now() - restartAt) / 1000 - 2, 180);
  await tunnel.page.close().catch(() => undefined);
  tunnel = null;
  assert.equal(await tunnelHttp(address, 30_000), 200, 'the Tailscale address serves after the restart');
  assert.equal(await localAnswer(mini.url), null, 'the local address stays closed after the restart');
  tunnel = await openTunnel();
  assert.deepEqual(await tunnel.rpc('getTailscaleOnlyAccess'), { enabled: true, throughTailscale: true });

  // Off: the local address answers again.
  assert.deepEqual(await tunnel.rpc('setTailscaleOnlyAccess', { enabled: false }), { enabled: false, throughTailscale: true });
  assert.equal(await localAnswer(mini.url), 200, 'the local address answers again');
});
