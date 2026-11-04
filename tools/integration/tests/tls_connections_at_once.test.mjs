import assert from 'node:assert/strict';
import net from 'node:net';
import { after, test } from 'node:test';
import { adoption, cloudKeysUrl, standInCloud } from '../lib/cloud.mjs';
import { closeDiskDialogs } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { mqttBroker, updateServer } from '../lib/servers.mjs';
import { MAX_FREEZE_MS, watchVideo } from '../lib/video.mjs';

// TLS admission reserves internal RAM against floors (jetkvm_memory_policy.h).
// Non-waiting owners (session open) are refused when it does not fit; cloud
// registration waits up to 2 s for other reservations.

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
// The Mini's full /webrtc/stats and, of it, one TLS owner's counts.
const stats = async mini => (await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(10_000) })).json();
const owner = (answer, name) => answer.memoryBudget.tlsAdmissions.find(one => one.owner === name) ?? { admitted: 0, denied: 0 };
const refusals = answer => Object.fromEntries(answer.memoryBudget.tlsAdmissions.filter(one => one.denied > 0).map(one => [one.owner, one.denied]));

// One stand-in cloud for the tests of this file (lib/cloud.mjs has why).
let cloud = null;
after(() => cloud?.close());
// Without run.sh no test skips: connect() then says how to start the tests.
const skip = process.env.INTEGRATION_BENCH && !cloudKeysUrl()
  ? 'the bench file sets no JETKVM_MOCK_CLOUD_JWKS_URL, so the Mini\'s build takes no login of the stand-in cloud' : false;

// What the Mini's /adopt page sends, from this machine and with the page's
// session left as it is. Returns { status, body }.
async function register(mini) {
  const origin = new URL(mini.url).origin;
  const answer = await fetch(`${origin}/cloud/register`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Origin: origin },
    body: JSON.stringify(cloud.adoption()),
    signal: AbortSignal.timeout(30_000),
  });
  return { status: answer.status, body: await answer.json().catch(() => null) };
}

// Off the cloud before a registration: the page offers "Adopt" only to a
// Mini that is on no cloud, so a registration never has the cloud socket
// beside it.
async function offCloud(mini) {
  await mini.rpc('deregisterDevice', {}, 30_000);
  await until(() => stats(mini), now => now.cloudConnected === false, 10);
}

// A port of this machine in front of host:port that holds back what the
// server sends for the first holdMs of each connection, so that a TLS
// handshake through it lasts that long. Returns { port, close }.
function slowFront(host, port, holdMs) {
  const sockets = new Set();
  const server = net.createServer(client => {
    const upstream = net.connect(port, host);
    const openAt = Date.now() + holdMs;
    let sent = Promise.resolve();
    for (const socket of [client, upstream]) {
      sockets.add(socket);
      socket.on('error', () => undefined);
      socket.on('close', () => {
        sockets.delete(socket);
        client.destroy();
        upstream.destroy();
      });
    }
    client.pipe(upstream);
    // One after the other, so that the held chunks keep their order.
    upstream.on('data', chunk => {
      sent = sent.then(() => sleep(Math.max(0, openAt - Date.now()))).then(() => { if (!client.destroyed) client.write(chunk); });
    });
  });
  return new Promise(resolve => server.listen(0, () => resolve({
    port: server.address().port,
    close: () => {
      for (const socket of sockets) socket.destroy();
      server.close();
    },
  })));
}

// The bench's Mini with MQTT over TLS at a broker of this test, connected.
// With handshakeMs each TLS handshake with the broker lasts that long.
// Returns the broker and the settings that the Mini has now; restore()
// stores the MQTT settings as found.
async function mqttOverTls(mini, { handshakeMs = 0 } = {}) {
  const found = await mini.rpc('getMqttSettings');
  const broker = await mqttBroker(mini, { secure: true });
  const front = handshakeMs ? await slowFront(broker.host, broker.port, handshakeMs) : null;
  const restore = async () => {
    try {
      await mini.rpc('setMqttSettings', { settings: found }, 30_000);
    } finally {
      front?.close();
      broker.close();
    }
  };
  // The broker's certificate is one that the Mini does not trust, as in the walk.
  const settings = { ...found, enabled: true, broker: broker.host, port: front?.port ?? broker.port,
    base_topic: 'tls-at-once', use_tls: true, tls_insecure: true };
  try {
    await mini.rpc('setMqttSettings', { settings }, 30_000);
    await until(() => mini.rpc('getMqttStatus'), status => status.connected === true, 30);
  } catch (error) {
    await restore().catch(() => undefined);
    throw error;
  }
  return { broker, settings, restore };
}

test('beside MQTT over TLS and a mounted HTTPS image, a cloud registration during an update check succeeds, video goes on, and a new session starts', { timeout: 420_000, skip }, async t => {
  const mini = await connect();
  let visit;
  let mqtt;
  let server;
  let slowBroker;
  let mounted = false;
  let updateApi;
  // Registered before the first check that can fail: the browser of
  // connect() closes also then, or the test's process does not end.
  t.after(async () => {
    try {
      await mini.open();
      if (mounted) {
        await mini.rpc('unmountImage', {}, 30_000).catch(() => undefined);
        await closeDiskDialogs(mini);
      }
      await mqtt?.restore();
      if (updateApi !== undefined) await mini.rpc('setUpdateApiUrl', { url: updateApi });
      await visit?.leave();
    } finally {
      await mini.close().finally(() => {
        server?.close();
        slowBroker?.close();
      });
    }
  });
  cloud ??= standInCloud();
  visit = await adoption(mini, cloud);
  updateApi = await mini.rpc('getUpdateApiUrl');
  if ((await mini.rpc('getUsbDevices')).mass_storage !== true) return t.skip('the Mini\'s USB devices have no drive (getUsbDevices)');
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'the test needs a Mini with nothing mounted');
  // What the Mini has refused before the test, per TLS owner.
  const refusedBefore = refusals(await stats(mini));

  // The three connections that stay open: the cloud socket, MQTT, the image.
  await visit.adopt();
  await until(() => stats(mini), now => now.cloudConnected === true, 15);
  mqtt = await mqttOverTls(mini);
  // The stand-in update server answers an update check and serves the image,
  // both over TLS with a certificate that a bench build trusts.
  server = await updateServer(mini);
  const { url: imageUrl } = await server.secure();
  await mini.rpc('setUpdateApiUrl', { url: new URL(imageUrl).origin });
  const beforeMount = await stats(mini);
  // A CD-ROM image for the Mini's CD-ROM drive keeps the Mini on USB.
  await mini.rpc('mountWithHTTP', { url: imageUrl, mode: 'CDROM' }, 60_000);
  mounted = true;
  // The host reads the new medium by itself; each read is one request on the image's open connection.
  await until(() => stats(mini), now => owner(now, 'https-media-range').admitted > owner(beforeMount, 'https-media-range').admitted, 30);
  await until(mini.frames, frames => frames > 0, 30);
  await sleep(3000);

  // A registration while another TLS connection holds its reservation for
  // certain: the test connection of the MQTT settings page, to a broker whose
  // handshake lasts 2.5 s. The Mini reports the held reservation before the
  // registration is sent and after it is answered.
  const HANDSHAKE_MS = 2500;
  await offCloud(mini);
  slowBroker = await slowFront(mqtt.broker.host, mqtt.broker.port, HANDSHAKE_MS);
  const holds = answer => answer.memoryBudget.activeTlsReservations > 0 && (owner(answer, 'mqtt-test').reservedDma ?? 1) > 0;
  const firstSockets = cloud.asked().sockets;
  const testing = mini.rpc('testMqttConnection', { settings: { ...mqtt.settings, port: slowBroker.port } }, 20_000)
    .then(answer => answer, error => ({ error: error.message }));
  await until(() => stats(mini), holds, 2);
  const during = await register(mini);
  const still = holds(await stats(mini));
  assert.equal(during.status, 200, `the Mini takes a registration while the MQTT test holds its reservation (${JSON.stringify(during.body)}; ${cloud.problem() ?? 'the cloud saw no problem'})`);
  assert.ok(still, 'the MQTT test still held its reservation when the registration was answered');
  const tested = await testing;
  assert.equal(tested.success, true, `the MQTT test connects (${JSON.stringify(tested)})`);
  await until(() => cloud.connected() && cloud.asked().sockets > firstSockets, Boolean, 30);
  await until(() => stats(mini), now => now.cloudConnected === true, 15);
  await offCloud(mini);
  await sleep(3000);

  const before = await stats(mini);
  const sockets = cloud.asked().sockets;
  const watch = watchVideo(mini);
  await sleep(2000);
  // The update check goes to the task of the session and holds its
  // reservation for its handshake of about 350 ms. The registration goes to
  // the HTTP server's task 100 ms later, so it asks for its reservation
  // while the check holds one.
  const from = Date.now();
  const checking = mini.rpc('getUpdateStatus', {}, 30_000)
    .then(answer => answer, error => ({ error: error.message })).then(answer => ({ answer, at: Date.now() }));
  await sleep(100);
  const registeredAt = Date.now();
  const registration = await register(mini);
  const { answer: check, at: checkedAt } = await checking;
  await sleep(2000);
  const freeze = watch();
  const now = await stats(mini);
  t.diagnostic(`the update check took ${checkedAt - from} ms, and the registration started ${checkedAt - registeredAt} ms before its answer; ` +
    `longest time without a new frame ${freeze.ms} ms; refused so far: ${JSON.stringify(refusals(now))}`);
  assert.ok(checkedAt - registeredAt >= 100, `the registration started ${checkedAt - registeredAt} ms before the update check's answer, during its handshake`);
  for (const name of ['ota-download', 'cloud-registration-tls', 'https-media-range', 'mqtt-connect', 'mqtt-test']) {
    const { reservedDma, heldDmaMin, heldDmaFallMax, heldOverruns } = owner(now, name);
    t.diagnostic(`${name}: holds ${reservedDma} B, least free DMA-capable RAM while it held ${heldDmaMin} B, largest fall ${heldDmaFallMax} B, overruns ${heldOverruns}`);
  }
  assert.equal(registration.status, 200, `the Mini takes the registration (${JSON.stringify(registration.body)}; ${cloud.problem() ?? 'the cloud saw no problem'})`);
  assert.equal(check.error, undefined, 'the Mini answers the update check');
  assert.ok(check.remote?.systemVersion, `the update server offers a release (${JSON.stringify(check)})`);
  for (const name of ['ota-download', 'cloud-registration-tls']) {
    assert.ok(owner(now, name).admitted > owner(before, name).admitted, `${name} took a reservation`);
  }
  for (const name of ['ota-download', 'cloud-registration-tls', 'https-media-range']) {
    assert.equal(owner(now, name).denied, owner(before, name).denied, `the Mini refused no request of ${name}`);
  }
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `no new frame for ${freeze.ms} ms; the limit is ${MAX_FREEZE_MS} ms`);
  // The Mini is at the cloud with its new registration, and MQTT stayed.
  await until(() => cloud.connected() && cloud.asked().sockets > sockets, Boolean, 30);
  assert.equal((await mini.rpc('getMqttStatus')).connected, true, 'MQTT stayed connected');

  // A new session, as after a reload of the page: its open takes a reservation of its own.
  const dtls = owner(await stats(mini), 'webrtc-dtls');
  const second = await mini.session();
  try {
    await until(async () => (await second.video())?.framesDecoded ?? 0, frames => frames > 0, 30);
    const after = owner(await stats(mini), 'webrtc-dtls');
    assert.ok(after.admitted > dtls.admitted, 'the new session took a reservation');
    assert.equal(after.denied, dtls.denied, 'the Mini refused no session open');
  } finally {
    await second.close();
  }
  const end = await stats(mini);
  for (const one of end.memoryBudget.tlsAdmissions) {
    assert.ok(!(one.heldOverruns > 0), `while ${one.owner} held a reservation, free memory fell by more than what was reserved and the room between the floors (${JSON.stringify(one)})`);
  }
  // Each step of this test is one or two TLS connections at a time beside
  // the open ones: the adoption with the page's new session, the MQTT
  // settings with their connect, the mount, the registrations, the update
  // check, the new session. The Mini refuses none of them.
  assert.deepEqual(refusals(end), refusedBefore, 'the Mini refused no TLS reservation during the test');
});

// The Mini's uptime in ms, or null while it does not answer.
const uptimeMs = mini => fetch(new URL('/webrtc/stats?bitrate=1', mini.url), { signal: AbortSignal.timeout(1500) })
  .then(async response => (await response.json()).nowMs, () => null);

// Asks for a restart and returns when the Mini answers again after it. The
// page leaves the Mini, as a browser that is at the cloud's sign-in page, so
// that no session opens beside the registration.
async function restart(mini) {
  const before = await uptimeMs(mini);
  const refused = mini.rpc('reboot', { force: true }, 5000)
    .then(() => null, e => (/RPC timeout for reboot|RPC data channel not available|has no RPC channel|context was destroyed/.test(e.message) ? null : e));
  await sleep(500);
  await mini.page.goto('about:blank');
  await until(() => uptimeMs(mini), now => now !== null && (before === null || now < before), 90).catch(async e => {
    throw (await refused) ?? new Error(`the Mini did not restart: ${e.message}`);
  });
}

// After boot, MQTT over TLS holds a reservation while connecting. Registering
// as soon as a reservation is held overlaps the two; a refusal would be a 503
// for the user. The broker's 1 s handshake makes reconnects overlap too.
test('a cloud registration beside the MQTT connect after a restart is taken, 10 times', { timeout: 1_500_000, skip }, async t => {
  const ROUNDS = 10;
  const HANDSHAKE_MS = 1000;
  const mini = await connect();
  let visit;
  let mqtt;
  // Registered before the first check that can fail: the browser of
  // connect() closes also then, or the test's process does not end.
  t.after(async () => {
    try {
      await mini.open();
      await mqtt?.restore();
      await visit?.leave();
    } finally {
      await mini.close();
    }
  });
  cloud ??= standInCloud();
  visit = await adoption(mini, cloud);
  mqtt = await mqttOverTls(mini, { handshakeMs: HANDSHAKE_MS });
  // The cloud's address, which the /adopt page sets before the registration.
  await mini.rpc('setCloudUrl', { apiUrl: cloud.url, appUrl: cloud.url });

  const rounds = [];
  for (let round = 1; round <= ROUNDS; round++) {
    await restart(mini);
    // At the first answer in which the Mini holds a TLS reservation
    // (memoryBudget.activeTlsReservations), 40 s after the start at the latest.
    let at = null;
    let beside = null;
    for (;;) {
      const answer = await stats(mini).catch(() => null);
      at = await uptimeMs(mini);
      if (answer?.memoryBudget.activeTlsReservations > 0) {
        beside = answer.memoryBudget.tlsAdmissions.filter(one => one.reservedDma > 0).map(one => one.owner).join(', ') || 'a TLS reservation';
      }
      if (beside || at >= 40_000) break;
      await sleep(20);
    }
    const answer = await register(mini);
    const after = await stats(mini);
    rounds.push({ round, uptimeMs: at, beside, status: answer.status, refused: owner(after, 'cloud-registration-tls').denied,
      mqtt: { connect: owner(after, 'mqtt-connect').admitted, reconnect: owner(after, 'mqtt-reconnect').admitted },
      room: owner(after, 'cloud-registration-tls').lastDmaHeadroom });
    t.diagnostic(JSON.stringify(rounds.at(-1)));
    // Off the cloud again, so that the next start has no cloud socket, as the Mini of a user who has not adopted it.
    await mini.open();
    if ((await mini.rpc('getCloudState')).connected) await mini.rpc('deregisterDevice', {}, 30_000);
  }
  const refused = rounds.filter(one => one.status !== 200);
  assert.deepEqual(refused, [], `the Mini refused ${refused.length} of ${ROUNDS} registrations`);
  const alone = rounds.filter(one => !one.beside);
  assert.deepEqual(alone, [], `${alone.length} of ${ROUNDS} registrations had no held TLS reservation beside them`);
});
