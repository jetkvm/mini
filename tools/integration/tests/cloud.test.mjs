import assert from 'node:assert/strict';
import net from 'node:net';
import { after, test } from 'node:test';
import { adoption, cloudKeysUrl, standInCloud } from '../lib/cloud.mjs';
import { keyboard } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';

// The update server that a Mini uses outside a test (JETKVM_OTA_DEFAULT_API_URL).
const UPDATE_SERVER = 'https://api.jetkvm.com';
// The room that an update check's TLS connection leaves above the Mini's
// floors of internal and of DMA-capable RAM: more than twice the 6.6 kB
// that one such connection takes (the walk's rule tlsReserveMin).
const TLS_RESERVE_BYTES = 16 * 1024;
// Left Shift alone types nothing on the host. The Mini reports it in the
// modifier byte of the keys that are down.
const LEFT_SHIFT = { key: 0xe1, bit: 0x02 };

// The Mini's full /webrtc/stats: cloudConnected, internalHeapFree and the
// counts of its TLS connections by owner (memoryBudget.tlsAdmissions).
const stats = async mini => (await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(10_000) })).json();

// One stand-in cloud for the tests of this file (lib/cloud.mjs has why),
// started by the first test that runs. The Mini's socket goes there over wss,
// as to the real cloud.
let cloud = null;
after(() => cloud?.close());
// Without run.sh no test skips: connect() then says how to start the tests.
const skip = process.env.INTEGRATION_BENCH && !cloudKeysUrl()
  ? 'the bench file sets no JETKVM_MOCK_CLOUD_JWKS_URL, so the Mini\'s build takes no login of the stand-in cloud' : false;

// The bench's Mini with what a test does with it at the cloud: register(),
// adopt(). The test's end takes the Mini off the cloud and stores its cloud
// addresses as found; beforeLeave(step) sets what the test does before that.
async function miniAtCloud(t) {
  const mini = await connect();
  let visit;
  let last = () => undefined;
  t.after(async () => {
    try {
      await last();
    } finally {
      await Promise.resolve(visit?.leave()).finally(() => mini.close());
    }
  });
  cloud ??= standInCloud();
  visit = await adoption(mini, cloud);
  return { mini, ...visit, beforeLeave: step => { last = step; } };
}

// The Mini's part of an adoption: the calls of its settings page and of its
// /adopt page, without the cloud's own pages between them.
test('the Mini registers with a cloud, reports the adoption and opens its socket there', { timeout: 180_000, skip }, async t => {
  const { mini, register } = await miniAtCloud(t);
  const { systemVersion } = await mini.rpc('getLocalVersion');
  const before = cloud.asked();

  const answer = await register();
  assert.equal(answer.status, 200, `${JSON.stringify(answer.body)}; ${cloud.problem()}`);
  // The cloud's app has one UI per firmware version; the Mini's is "-mini".
  assert.equal(answer.body.cloudUiVersion, `${systemVersion}-mini`);
  assert.equal(cloud.asked().tokenExchanges, before.tokenExchanges + 1, 'the Mini exchanged the token once');
  // The cloud's key is new with the cloud, so the Mini does not hold it.
  assert.ok(cloud.asked().keyFetches > before.keyFetches, 'the Mini fetched the key set to check the login');
  assert.deepEqual(await mini.rpc('getCloudState'), { connected: true, url: cloud.url, appUrl: cloud.url });
  // The Mini opens its socket with the token that it was given.
  await until(() => cloud.connected() && cloud.asked().sockets > before.sockets, Boolean, 30);
  assert.equal(cloud.asked().appVersion, `${systemVersion}-mini`);
});

test('the Mini opens a new socket within 30 seconds after the cloud drops its socket', { timeout: 180_000, skip }, async t => {
  const { adopt } = await miniAtCloud(t);
  await adopt();

  const { sockets } = cloud.asked();
  const droppedAt = Date.now();
  cloud.drop();
  await until(() => cloud.asked().sockets, count => count > sockets, 30);
  t.diagnostic(`a new socket ${cloud.socketAt() - droppedAt} ms after the drop`);
  assert.equal(cloud.connected(), true);
});

// The cloud's address is one that accepts a TCP connection and never
// answers. Each connect of the Mini then waits in its TLS handshake for the
// Mini's network timeout of 10 seconds, and the Mini connects again 5
// seconds later.
test('a local session opens within 10 seconds and answers each call within 1 second while the cloud never answers', { timeout: 240_000, skip }, async t => {
  const { mini, adopt, beforeLeave } = await miniAtCloud(t);
  await adopt();
  const taken = new Set();
  let connections = 0;
  const silent = net.createServer(socket => {
    connections++;
    taken.add(socket);
    socket.on('error', () => undefined);
    socket.on('close', () => taken.delete(socket));
  });
  let keys;
  beforeLeave(async () => {
    await keys?.releaseAll().catch(() => undefined);
    for (const socket of taken) socket.destroy();
    silent.close();
  });
  await new Promise(resolve => silent.listen(0, resolve));
  await mini.rpc('setCloudUrl', { apiUrl: `https://${new URL(cloud.url).hostname}:${silent.address().port}`, appUrl: cloud.url });
  await until(() => connections, count => count > 0, 30);

  // A new session while that connect waits, then a call every 250 ms for 20 s.
  const loadAt = Date.now();
  await mini.open();
  const sessionMs = Date.now() - loadAt;
  const calls = [];
  for (const end = Date.now() + 20_000; Date.now() < end;) {
    const sentAt = Date.now();
    await mini.rpc('getDeviceID', {}, 5000);
    calls.push(Date.now() - sentAt);
    await new Promise(resolve => setTimeout(resolve, 250));
  }
  const frames = await mini.frames();
  await until(mini.frames, now => now > frames, 15);
  keys = await keyboard(mini);
  for (const press of [true, false]) {
    await keys.press(LEFT_SHIFT.key, press);
    await until(keys.down, down => ((down.modifier & LEFT_SHIFT.bit) !== 0) === press, 3);
  }
  t.diagnostic(`a session after ${sessionMs} ms; ${calls.length} calls, the slowest ${Math.max(...calls)} ms; ${connections} connections to the silent cloud`);
  assert.ok(sessionMs < 10_000, `the session took ${sessionMs} ms`);
  assert.ok(Math.max(...calls) < 1000, `the slowest call took ${Math.max(...calls)} ms`);
});

// An update check takes the TLS admission of a firmware download
// ("ota-download"), beside the TLS session of the cloud socket, which stays
// open while the Mini runs. The walk has this rule for a check without the
// cloud (tlsReserveMin).
test('an update check beside the cloud socket is admitted and leaves 16 KiB above the Mini\'s memory floors', { timeout: 180_000, skip }, async t => {
  const { mini, adopt } = await miniAtCloud(t);
  await adopt();
  await until(() => stats(mini), now => now.cloudConnected === true, 10);
  await until(mini.frames, frames => frames > 0, 30);
  const { sockets } = cloud.asked();
  // close() stores the update server that connect() found.
  if (await mini.rpc('getUpdateApiUrl') !== UPDATE_SERVER) await mini.rpc('setUpdateApiUrl', { url: UPDATE_SERVER });
  const admissions = async () => (await stats(mini)).memoryBudget.tlsAdmissions.find(({ owner }) => owner === 'ota-download') ?? { admitted: 0, denied: 0 };

  const before = await admissions();
  const answer = await mini.rpc('getUpdateStatus', {}, 30_000);
  const after = await admissions();
  t.diagnostic(`the check left ${after.lastInternalHeadroom} B of internal and ${after.lastDmaHeadroom} B of DMA RAM above the floors`);
  assert.equal(after.denied, before.denied, 'the Mini refused no check');
  assert.ok(after.admitted > before.admitted, 'the check took the admission of a firmware download');
  assert.ok(answer.remote?.systemVersion, 'the update server offers a release');
  assert.ok(after.lastInternalHeadroom >= TLS_RESERVE_BYTES, `internal RAM: ${after.lastInternalHeadroom} B`);
  assert.ok(after.lastDmaHeadroom >= TLS_RESERVE_BYTES, `DMA RAM: ${after.lastDmaHeadroom} B`);
  assert.equal(cloud.asked().sockets, sockets, 'the Mini kept its cloud socket');
  assert.equal(cloud.connected(), true);
});

// TLS admission is against free internal RAM, so the socket's footprint comes
// out of other TLS users. Cost = free RAM after deregistering minus before;
// each level is the max of ten readings, since transient use only lowers one.
test('the open cloud socket takes less than 4 KiB of internal RAM', { timeout: 180_000, skip }, async t => {
  const { mini, adopt } = await miniAtCloud(t);
  await adopt();
  await until(() => stats(mini), now => now.cloudConnected === true, 10);
  await until(mini.frames, frames => frames > 0, 30);
  const freeInternal = async () => {
    let highest = 0;
    for (let reading = 0; reading < 10; reading++) {
      if (reading > 0) await new Promise(resolve => setTimeout(resolve, 1000));
      highest = Math.max(highest, (await stats(mini)).internalHeapFree);
    }
    return highest;
  };

  const connected = await freeInternal();
  await mini.rpc('deregisterDevice', {}, 30_000);
  await until(() => stats(mini), now => now.cloudConnected === false, 10);
  const cost = await freeInternal() - connected;
  t.diagnostic(`the cloud socket takes ${cost} B of internal RAM`);
  assert.ok(cost < 4 * 1024, `the cloud socket takes ${cost} B`);
});
