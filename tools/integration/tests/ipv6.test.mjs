import assert from 'node:assert/strict';
import { test } from 'node:test';
import { pageRpc } from '../../combined_load/runner/page.mjs';
import { connect, until } from '../lib/mini.mjs';

// ipv6_mode gives the interface IPv6 addresses beside IPv4. The web server
// answers at them. The video connection of a session that the browser opens
// at the IPv6 address uses IPv4: the Mini offers only IPv4 candidates.
const stats = async url => (await fetch(new URL('/webrtc/stats', url), { signal: AbortSignal.timeout(5000) })).json();
// Node's fetch gives a refused connection a TypeError and no status.
const answers = async url => {
  try {
    return (await fetch(new URL('/device/status', url), { signal: AbortSignal.timeout(3000) })).status;
  } catch {
    return 0;
  }
};

test('with ipv6_mode slaac the Mini serves its page and a session at its IPv6 address, and video uses IPv4', { timeout: 360_000 }, async t => {
  const mini = await connect();
  // close() stores the network settings as found.
  t.after(() => mini.close());
  if (!(await mini.rpc('getDeviceCapabilities')).includes('ipv6')) return t.skip('the Mini has no IPv6 setting');
  const found = await mini.rpc('getNetworkSettings');
  const mode = ipv6_mode => mini.rpc('setNetworkSettings', { settings: { ...found, ipv6_mode } });

  // Off: no address of either kind, whatever the Mini was left with.
  await mode('disabled');
  const off = await until(() => mini.rpc('getNetworkState'),
    state => !state.ipv6_link_local && state.ipv6_addresses.length === 0, 15);
  assert.equal(off.ipv6_ready, false);

  // Link-local only: the routers' prefixes are not used.
  await mode('link_local');
  const local = await until(() => mini.rpc('getNetworkState'), state => state.ipv6_link_local, 15);
  assert.match(local.ipv6_link_local, /^fe80:[0-9a-f:]+$/);
  await new Promise(resolve => setTimeout(resolve, 8000));
  assert.equal((await mini.rpc('getNetworkState')).ipv6_addresses.length, 0,
    'link_local took an address from a router advertisement');

  // The UI's other modes are refused, and the mode stays.
  for (const refused of ['static', 'dhcpv6']) {
    await assert.rejects(mode(refused), /not supported/);
  }
  assert.equal((await mini.rpc('getNetworkSettings')).ipv6_mode, 'link_local');

  await mode('slaac');
  const state = await until(() => mini.rpc('getNetworkState'), now => now.ipv6_addresses.length > 0, 30);
  assert.match(state.ipv6_link_local, /^fe80:[0-9a-f:]+$/);
  const address = state.ipv6_addresses[0].address;
  assert.match(address, /^[0-9a-f:]+$/, 'the address is not in lower case');
  const url = `http://[${address}]/`;
  t.diagnostic(`IPv6 addresses: ${state.ipv6_addresses.map(one => one.address).join(', ')}; link-local ${state.ipv6_link_local}`);
  assert.equal(await until(() => answers(url), status => status === 200, 15), 200);
  // IPv4 is as before.
  assert.equal(await answers(mini.url), 200);
  assert.equal(state.ipv4_ready, true);

  // A session at the IPv6 address. The first page must be gone, so that the
  // connected peer is the page below.
  await mini.leave();
  const page = await mini.page.context().browser().newPage();
  try {
    await page.addInitScript(() => {
      const Original = window.RTCPeerConnection;
      window.__peers = [];
      window.RTCPeerConnection = class extends Original {
        constructor(...values) {
          super(...values);
          window.__peers.push(this);
        }
      };
    });
    const rpc = pageRpc(page);
    await until(() => page.goto(url, { waitUntil: 'domcontentloaded', timeout: 10_000 }), () => true, 60);
    assert.equal(typeof await until(() => rpc('getDeviceID'), () => true, 60), 'string');
    assert.equal((await stats(mini.url)).peerConnected, true);
    // The selected pair of the page's connection: which address family the
    // video and the input use.
    const pair = await until(() => page.evaluate(async () => {
      const peer = (window.__peers ?? []).find(one => one.connectionState === 'connected');
      if (!peer) return null;
      const report = await peer.getStats();
      let selected = null;
      report.forEach(entry => {
        if (entry.type === 'candidate-pair' && entry.nominated && entry.state === 'succeeded') selected = entry;
      });
      if (!selected) return null;
      const remote = report.get(selected.remoteCandidateId);
      return { remote: remote.address, type: remote.candidateType };
    }), value => value, 20);
    t.diagnostic(`the page at ${url} reaches the Mini's video port at ${pair.remote} (${pair.type})`);
    assert.match(pair.remote, /^\d+\.\d+\.\d+\.\d+$/, 'the video connection does not use IPv4');
    // Video arrives on that connection when the Mini has a picture.
    const picture = (await stats(mini.url)).width > 0;
    if (picture) {
      const frames = () => page.evaluate(async () => (await window.__kvmTestHooks?.getInboundVideoStats?.())?.framesDecoded ?? 0);
      const first = await frames();
      await until(frames, now => now > first + 10, 20);
    } else {
      t.diagnostic('the Mini has no picture, so no video frame was counted');
    }
  } finally {
    await page.close();
  }

  // The mode is stored: after a restart the Mini has the address again.
  await mini.open();
  const askedAt = Date.now();
  await mini.send('reboot', { force: true });
  let misses = 0;
  await until(async () => {
    misses = await answers(mini.url) === 200 ? 0 : misses + 1;
    return misses;
  }, count => count >= 3, 30);
  await mini.open();
  assert.equal(await mini.restarted(askedAt), true, 'the Mini has restarted');
  const restarted = await until(() => mini.rpc('getNetworkState'), now => now.ipv6_addresses.length > 0, 40);
  assert.equal(restarted.ipv6_addresses[0].address, address);
  assert.equal(await until(() => answers(url), status => status === 200, 15), 200);

  // Off again: the IPv6 address stops answering and IPv4 goes on.
  await mode('disabled');
  await until(() => mini.rpc('getNetworkState'),
    now => !now.ipv6_link_local && now.ipv6_addresses.length === 0, 15);
  assert.equal(await until(() => answers(url), status => status === 0, 15), 0);
  assert.equal(await answers(mini.url), 200);
});
