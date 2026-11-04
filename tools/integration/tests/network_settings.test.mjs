import assert from 'node:assert/strict';
import { isIPv4 } from 'node:net';
import { test } from 'node:test';
import { isDeepStrictEqual } from 'node:util';
import { connect, until } from '../lib/mini.mjs';

const HOSTNAME = 'jetkvm-e2e-network';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// The test changes the network of the connection that it uses. The Mini
// stays reachable because the static address is the address that it has, with
// the netmask of its network; without a gateway it answers this machine only
// when both are in one network, as on a bench.
test('a static IPv4 address without a gateway and DNS servers is stored, and a new hostname with it', { timeout: 420_000 }, async t => {
  const mini = await connect();
  // The settings as found, from the moment at which the test may change them.
  let original = null;
  t.after(async () => {
    try {
      if (original === null) return;
      // The Mini sets its interface up again for new settings: it can miss
      // the answer to the call, and it takes the call again after that.
      await until(async () => {
        await mini.open();
        if (isDeepStrictEqual(await mini.rpc('getNetworkSettings'), original)) return true;
        await mini.rpc('setNetworkSettings', { settings: original }).catch(() => undefined);
        await sleep(5000);
        return false;
      }, stored => stored, 240);
    } finally {
      await mini.close();
    }
  });

  const address = new URL(mini.url).hostname;
  if (!isIPv4(address)) {
    t.skip('needs an IPv4 address of the Mini in the bench file (JETKVM_URL)');
    return;
  }
  const startedAt = Date.now();
  const found = await mini.rpc('getNetworkSettings');
  const state = await mini.rpc('getNetworkState');
  const netmask = state.dhcp_lease?.netmask || found.ipv4_static?.netmask;
  assert.ok(netmask, 'the netmask of the Mini\'s network, without which the Mini would not stay reachable');

  t.diagnostic(`found ${found.ipv4_mode}, hostname ${found.hostname}; static ${address} ${netmask}`);
  original = found;
  await mini.rpc('setNetworkSettings',
    { settings: { ...found, hostname: HOSTNAME, ipv4_mode: 'static', ipv4_static: { address, netmask, gateway: '', dns: [] } } });
  await sleep(5000);
  const appliedAt = Date.now();
  await mini.open();
  assert.ok(Date.now() - appliedAt <= 90_000, 'the Mini takes calls again within 90 seconds');

  const saved = await mini.rpc('getNetworkSettings');
  assert.equal(saved.hostname, HOSTNAME);
  assert.equal(saved.ipv4_mode, 'static');
  assert.equal(saved.ipv4_static?.address, address);
  assert.equal(saved.ipv4_static?.netmask, netmask);
  assert.equal(saved.ipv4_static?.gateway ?? '', '', 'no gateway is stored');
  assert.deepEqual(saved.ipv4_static?.dns ?? [], [], 'no DNS server is stored');
  await until(() => mini.rpc('getNetworkState'), now => now.hostname === HOSTNAME, 5);
  assert.ok(Date.now() - startedAt <= 180_000, 'the test takes 180 seconds at most');
});
