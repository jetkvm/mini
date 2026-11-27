import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { test } from 'node:test';
import { pageRpc } from '../../combined_load/runner/page.mjs';
import { connect, until } from '../lib/mini.mjs';

// ipv6_mode gives the interface IPv6 addresses beside IPv4. The web server
// answers at them, a session offers the browser an IPv6 address for video and
// input, and the mDNS responder announces them unless its mode is ipv4_only.
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const stats = async url => (await fetch(new URL('/webrtc/stats', url), { signal: AbortSignal.timeout(5000) })).json();
// Node's fetch gives a refused connection a TypeError and no status.
const answers = async url => {
  try {
    return (await fetch(new URL('/device/status', url), { signal: AbortSignal.timeout(3000) })).status;
  } catch {
    return 0;
  }
};

// This machine's address on the route to `host`.
const addressTowards = host => new Promise((resolve, reject) => {
  const socket = dgram.createSocket('udp4');
  socket.once('error', reject);
  socket.connect(9, host, () => {
    const { address } = socket.address();
    socket.close(() => resolve(address));
  });
});

// A name of a DNS message at `offset`: [name, the offset after it].
function dnsName(message, offset) {
  const labels = [];
  let after = null;
  for (;;) {
    const length = message[offset];
    if (length === 0) return [labels.join('.'), after ?? offset + 1];
    if ((length & 0xc0) === 0xc0) {
      after ??= offset + 2;
      offset = ((length & 0x3f) << 8) | message[offset + 1];
    } else {
      labels.push(message.toString('latin1', offset + 1, offset + 1 + length));
      offset += 1 + length;
    }
  }
}

// The A and AAAA records that the link's mDNS responders give for `name`
// within `seconds`, asked as a querier asks: from port 5353 to 224.0.0.251.
async function mdnsAddresses(name, local, seconds = 4) {
  const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true, reusePort: true });
  const found = { a: new Set(), aaaa: new Set() };
  socket.on('message', message => {
    if (!(message.readUInt16BE(2) & 0x8000)) return;
    let offset = 12;
    for (let i = 0; i < message.readUInt16BE(4); i++) offset = dnsName(message, offset)[1] + 4;
    const records = message.readUInt16BE(6) + message.readUInt16BE(8) + message.readUInt16BE(10);
    for (let i = 0; i < records; i++) {
      const [owner, next] = dnsName(message, offset);
      const type = message.readUInt16BE(next);
      const size = message.readUInt16BE(next + 8);
      const data = message.subarray(next + 10, next + 10 + size);
      offset = next + 10 + size;
      if (owner.toLowerCase() !== name.toLowerCase()) continue;
      if (type === 1) found.a.add([...data].join('.'));
      if (type === 28) {
        found.aaaa.add(Array.from({ length: 8 }, (_, group) => data.readUInt16BE(group * 2).toString(16)).join(':'));
      }
    }
  });
  await new Promise((resolve, reject) => socket.once('error', reject).bind(5353, resolve));
  try {
    socket.addMembership('224.0.0.251', local);
    socket.setMulticastInterface(local);
    const question = type => Buffer.concat([
      ...name.split('.').map(label => Buffer.concat([Buffer.from([label.length]), Buffer.from(label, 'latin1')])),
      Buffer.from([0, 0, type, 0, 1]),
    ]);
    const query = Buffer.concat([Buffer.from([0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0]), question(1), question(28)]);
    for (let sent = 0; sent < 3; sent++) {
      socket.send(query, 5353, '224.0.0.251');
      await sleep(seconds * 1000 / 3);
    }
  } finally {
    socket.close();
  }
  return { a: [...found.a], aaaa: [...found.aaaa] };
}
// An address as mdnsAddresses() writes it: eight groups, no "::".
const groups = address => {
  const [head, tail = ''] = address.split('::');
  const left = head ? head.split(':') : [];
  const right = tail ? tail.split(':') : [];
  const middle = address.includes('::') ? Array(8 - left.length - right.length).fill('0') : [];
  return [...left, ...middle, ...right].map(group => parseInt(group, 16).toString(16)).join(':');
};

// The page's RTCPeerConnections, for its selected candidate pair.
const keepPeers = () => {
  const Original = window.RTCPeerConnection;
  window.__peers = [];
  window.RTCPeerConnection = class extends Original {
    constructor(...values) {
      super(...values);
      window.__peers.push(this);
    }
  };
};
// The address at the Mini that the page's connected peer sends to.
const selectedRemote = page => until(() => page.evaluate(async () => {
  const peer = (window.__peers ?? []).find(one => one.connectionState === 'connected');
  if (!peer) return null;
  const report = await peer.getStats();
  let selected = null;
  report.forEach(entry => {
    if (entry.type === 'candidate-pair' && entry.nominated && entry.state === 'succeeded') selected = entry;
  });
  return selected ? report.get(selected.remoteCandidateId).address : null;
}), value => value, 20);

test('ipv6_mode gives the Mini IPv6 addresses for its page, its sessions and its mDNS name', { timeout: 480_000 }, async t => {
  const mini = await connect();
  // close() stores the network settings as found.
  t.after(() => mini.close());
  const capabilities = await mini.rpc('getDeviceCapabilities');
  if (!capabilities.includes('ipv6')) return t.skip('the Mini has no IPv6 setting');
  const found = await mini.rpc('getNetworkSettings');
  const set = values => mini.rpc('setNetworkSettings', { settings: { ...found, mdns_mode: 'auto', ...values } });
  const state = () => mini.rpc('getNetworkState');
  const own = new URL(mini.url).hostname;
  const local = await addressTowards(own);
  const name = `${found.hostname.toLowerCase()}.local`;
  // The records settle a few seconds after a change.
  const mdns = check => until(() => mdnsAddresses(name, local), check, 30);

  // Off: no address of either kind, whatever the Mini was left with.
  await set({ ipv6_mode: 'disabled' });
  const off = await until(state, now => !now.ipv6_link_local && now.ipv6_addresses.length === 0, 15);
  assert.equal(off.ipv6_ready, false);
  assert.deepEqual((await mdns(records => records.a.length > 0)).aaaa, [], 'an AAAA record without an IPv6 address');

  // Link-local only: the routers' prefixes are not used.
  await set({ ipv6_mode: 'link_local' });
  const linkLocal = (await until(state, now => now.ipv6_link_local, 15)).ipv6_link_local;
  assert.match(linkLocal, /^fe80:[0-9a-f:]+$/);
  await sleep(8000);
  assert.equal((await state()).ipv6_addresses.length, 0, 'link_local took an address from a router advertisement');

  // Upstream's other modes are refused, and the mode stays.
  for (const ipv6_mode of ['static', 'dhcpv6']) await assert.rejects(set({ ipv6_mode }), /not supported/);
  assert.equal((await mini.rpc('getNetworkSettings')).ipv6_mode, 'link_local');

  await set({ ipv6_mode: 'slaac' });
  const now = await until(state, value => value.ipv6_addresses.length > 0, 30);
  const address = now.ipv6_addresses[0].address;
  assert.match(address, /^[0-9a-f:]+$/, 'the address is not in lower case');
  assert.equal(now.ipv6_link_local, linkLocal);
  const url = `http://[${address}]/`;
  t.diagnostic(`IPv6 addresses: ${now.ipv6_addresses.map(one => one.address).join(', ')}; link-local ${linkLocal}`);
  assert.equal(await until(() => answers(url), status => status === 200, 15), 200);
  // IPv4 is as before.
  assert.equal(await answers(mini.url), 200);
  assert.equal(now.ipv4_ready, true);

  // mDNS: "auto" announces the IPv6 addresses, "ipv4_only" does not, and
  // "auto" brings them back.
  const announced = records => records.aaaa.includes(groups(address)) && records.a.includes(own);
  await mdns(announced);
  await set({ ipv6_mode: 'slaac', mdns_mode: 'ipv4_only' });
  await mdns(records => records.a.includes(own) && records.aaaa.length === 0);
  // Not only at that moment: the records stay out.
  await sleep(5000);
  assert.deepEqual((await mdnsAddresses(name, local)).aaaa, [], 'ipv4_only announced an IPv6 address');
  await set({ ipv6_mode: 'slaac', mdns_mode: 'auto' });
  await mdns(announced);

  // The first page must be gone, so that a connected peer is a page below.
  await mini.leave();
  const browser = mini.page.context().browser();
  const picture = (await stats(mini.url)).width > 0;
  const session = async route => {
    const page = await browser.newPage();
    try {
      await page.addInitScript(keepPeers);
      if (route) await route(page);
      const rpc = pageRpc(page);
      await until(() => page.goto(url, { waitUntil: 'domcontentloaded', timeout: 10_000 }), () => true, 60);
      assert.equal(typeof await until(() => rpc('getDeviceID'), () => true, 60), 'string');
      assert.equal((await stats(mini.url)).peerConnected, true);
      const remote = await selectedRemote(page);
      // Video arrives on that connection when the Mini has a picture.
      if (picture) {
        const frames = () => page.evaluate(async () => (await window.__kvmTestHooks?.getInboundVideoStats?.())?.framesDecoded ?? 0);
        const first = await frames();
        await until(frames, count => count > first + 10, 20);
      }
      return remote;
    } finally {
      await page.close();
      await until(() => stats(mini.url), value => value.sessionActive === false && value.peerConnected === false, 15);
    }
  };
  if (!picture) t.diagnostic('the Mini has no picture, so no video frame was counted');

  // A session at the IPv6 address, with both address families to choose from.
  t.diagnostic(`a page at ${url} sends to ${await session()}`);

  // A browser that reaches the Mini over IPv6 only. Its stand-in is a page
  // that gets an address of the documentation range (192.0.2.1) in place of
  // the Mini's IPv4 address in each candidate, so that IPv6 is its only path.
  let replaced = 0;
  const remote = await session(page => page.routeWebSocket(/\/webrtc\/signaling\/client/, socket => {
    const device = socket.connectToServer();
    socket.onMessage(message => device.send(message));
    device.onMessage(message => {
      if (typeof message !== 'string' || !message.startsWith('{')) return socket.send(message);
      const signal = JSON.parse(message);
      if (signal.type !== 'answer') {
        const text = message.replaceAll(own, '192.0.2.1');
        if (text !== message) replaced++;
        return socket.send(text);
      }
      const packed = typeof signal.data === 'string' ? signal.data : signal.data.sd;
      const description = JSON.parse(Buffer.from(packed, 'base64').toString());
      const sdp = description.sdp.replaceAll(own, '192.0.2.1');
      if (sdp !== description.sdp) replaced++;
      const again = Buffer.from(JSON.stringify({ ...description, sdp })).toString('base64');
      socket.send(JSON.stringify({ ...signal, data: typeof signal.data === 'string' ? again : { ...signal.data, sd: again } }));
    });
  }));
  t.diagnostic(`a page without a path to the Mini's IPv4 address sends to ${remote}`);
  assert.ok(replaced > 0, 'the Mini signaled no IPv4 candidate, so the test replaced nothing');
  assert.equal(groups(remote), groups(address), 'the video connection does not use the Mini\'s IPv6 address');

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
  const restarted = await until(state, value => value.ipv6_addresses.length > 0, 40);
  assert.equal(restarted.ipv6_addresses[0].address, address);
  assert.equal(await until(() => answers(url), status => status === 200, 15), 200);

  // Off again: the IPv6 address stops answering and IPv4 goes on.
  await set({ ipv6_mode: 'disabled' });
  await until(state, value => !value.ipv6_link_local && value.ipv6_addresses.length === 0, 15);
  assert.equal(await until(() => answers(url), status => status === 0, 15), 0);
  assert.equal(await answers(mini.url), 200);
});
