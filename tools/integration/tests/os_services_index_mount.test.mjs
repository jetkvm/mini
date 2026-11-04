import assert from 'node:assert/strict';
import https from 'node:https';
import net from 'node:net';
import { test } from 'node:test';
import { serverCertificate } from '../../combined_load/runner/fake_cloud.mjs';
import { addressTowards } from '../../combined_load/runner/switches.mjs';
import { closeDiskDialogs } from '../lib/host.mjs';
import { connect, mockCloudBuild } from '../lib/mini.mjs';
import { openSession } from '../lib/os_services.mjs';
import { senderHost } from '../lib/sender_host.mjs';
import { updateServer } from '../lib/servers.mjs';

// How long the stand-in bucket holds each TLS handshake, so that the mount
// starts while the refresh holds its reservation.
const HANDSHAKE_DELAY_MS = 1000;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// An HTTPS server that answers 404 to everything, after a slow handshake.
// connections has the time of each connection.
async function slowBucket(address) {
  const tls = https.createServer(serverCertificate(address), (request, response) => response.writeHead(404, { 'Content-Length': 0 }).end());
  const connections = [];
  const server = net.createServer(socket => {
    connections.push(Date.now());
    socket.on('error', () => undefined);
    setTimeout(() => tls.emit('connection', socket), HANDSHAKE_DELAY_MS);
  });
  await new Promise(resolve => server.listen(0, resolve));
  return {
    base: `https://${address}:${server.address().port}`,
    connections,
    close: () => {
      tls.closeAllConnections();
      server.close();
    },
  };
}

// A channel change refreshes the release list over TLS; an HTTPS mount
// started during that (slowed) handshake must get its own TLS reservation,
// waiting up to 2 s for room. Needs a mock-cloud build.
test('a mount of an HTTPS image during the handshake of the release list refresh succeeds', { timeout: 300_000 }, async t => {
  const mini = await connect();
  let session;
  let bucket;
  let images;
  t.after(async () => {
    try {
      await mini.rpc('unmountImage', {}, 30_000).catch(() => undefined);
      await closeDiskDialogs(mini);
      await session?.restore();
    } finally {
      // close() stores the update server's address and the channel again.
      await mini.close().finally(() => {
        bucket?.close();
        images?.close();
      });
    }
  });
  if (!mockCloudBuild(mini.image.version)) {
    return t.skip(`${mini.image.version} is no bench build with the mock cloud, which reads the release bucket at the update server's address`);
  }
  if ((await mini.rpc('getUsbDevices')).mass_storage !== true) return t.skip('the Mini\'s USB devices have no drive (getUsbDevices)');
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'the test needs a Mini with nothing mounted');
  // One owner's TLS reservations since the Mini's start.
  const tickets = async owner => {
    const stats = await (await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(10_000) })).json();
    const { admitted = 0, denied = 0 } = stats.memoryBudget.tlsAdmissions.find(one => one.owner === owner) ?? {};
    return { admitted, denied };
  };

  session = await openSession(mini, await senderHost(mini));
  images = await updateServer(mini);
  const { url: image } = await images.secure();
  bucket = await slowBucket(await addressTowards(new URL(mini.url).hostname));
  await mini.rpc('setUpdateApiUrl', { url: bucket.base });

  const refreshes = (await tickets('osvc-update')).admitted;
  const checks = await tickets('https-media-check');
  const connected = bucket.connections.length;
  await mini.rpc('setDevChannelState', { enabled: !(await mini.rpc('getDevChannelState')) });
  for (const end = Date.now() + 15_000; bucket.connections.length === connected; await sleep(20)) {
    assert.ok(Date.now() < end, 'the refresh connects to the bucket within 15 s');
  }
  assert.ok(Date.now() < bucket.connections.at(-1) + HANDSHAKE_DELAY_MS, 'the mount starts during the handshake of the refresh');
  await mini.rpc('mountWithHTTP', { url: image, mode: 'CDROM' }, 60_000);
  assert.equal((await mini.rpc('getVirtualMediaState'))?.source, 'HTTP');
  assert.equal((await tickets('osvc-update')).admitted, refreshes + 1, 'the refresh took its reservation');
  const now = await tickets('https-media-check');
  assert.equal(now.admitted, checks.admitted + 1, 'the check of the mount got its reservation');
  assert.equal(now.denied, checks.denied, 'the check of the mount was not refused');
});
