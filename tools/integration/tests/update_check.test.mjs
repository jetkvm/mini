import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import http from 'node:http';
import { test } from 'node:test';
import { addressTowards } from '../../combined_load/runner/switches.mjs';
import { firstFrame } from '../../combined_load/runner/takeover.mjs';
import { connect, until } from '../lib/mini.mjs';

const PRODUCTION = 'https://api.jetkvm.com';
const SKUS = ['jetkvm-mini-ethernet', 'jetkvm-mini-wireless'];
const STABLE = '98.0.0';
const PRERELEASE = '99.0.0-dev202610130000';

// What the Mini asks its update server, and what it makes of the answer.
// The update server of these tests answers the Mini's release request with
// answer(query), { status, body }, and keeps each request as { query, body }.
// `files` are the other paths that it serves, by path.
async function releaseServer(mini, answer, files = {}) {
  const asked = [];
  const server = http.createServer((request, response) => {
    const { pathname, searchParams } = new URL(request.url, 'http://localhost');
    if (pathname === '/releases') {
      const query = Object.fromEntries(searchParams);
      const { status = 200, body } = answer(query);
      asked.push({ query, body });
      response.writeHead(status, { 'Content-Type': 'application/json' }).end(JSON.stringify(body));
    } else if (files[pathname]) {
      response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': files[pathname].length }).end(files[pathname]);
    } else {
      response.writeHead(404).end();
    }
  });
  await new Promise(resolve => server.listen(0, resolve));
  const base = `http://${await addressTowards(new URL(mini.url).hostname)}:${server.address().port}`;
  const close = () => {
    server.closeAllConnections();
    server.close();
  };
  return { base, asked, close };
}

// A release of `version`. A check reads the answer and takes no image.
const release = (base, version) => ({ systemVersion: version, systemUrl: `${base}/jetkvm-mini.bin`, systemHash: '0'.repeat(64) });

// The Mini, pointed at an update server with these answers.
async function miniWithServer(t, answer, files) {
  const mini = await connect();
  let server;
  const startedAt = Date.now();
  t.after(async () => {
    // An update that a test started ends before the bench is given back.
    await until(async () => (await mini.restarted(startedAt)) || (await mini.rpc('isUpdatePending')) === false, Boolean, 180)
      .catch(() => undefined);
    await mini.close().finally(() => server?.close());
  });
  server = await releaseServer(mini, query => answer(query, server.base), files);
  await mini.rpc('setUpdateApiUrl', { url: server.base });
  return { mini, server };
}

test('the release request names the Mini, its board and its channel, and the Mini reports the release of that channel', { timeout: 180_000 }, async t => {
  const { mini, server } = await miniWithServer(t,
    (query, base) => ({ body: release(base, query.prerelease === 'true' ? PRERELEASE : STABLE) }));
  for (const [devChannel, version] of [[true, PRERELEASE], [false, STABLE]]) {
    await mini.rpc('setDevChannelState', { enabled: devChannel });
    const before = server.asked.length;
    const status = await mini.rpc('getUpdateStatus', {}, 30_000);
    assert.ok(server.asked.length > before, 'the Mini asked its update server');
    const { query, body } = server.asked.at(-1);
    assert.match(query.deviceId, /^[0-9a-f]{12}$/);
    assert.ok(SKUS.includes(query.sku), `sku=${query.sku}`);
    assert.equal(query.prerelease, String(devChannel));
    assert.equal(body.systemVersion, version, 'the server answered with the release of the channel');
    assert.equal(status.remote?.systemVersion, version);
  }
});

test('a check that the update server answers with 503 is an error, not "no update"', { timeout: 120_000 }, async t => {
  const { mini, server } = await miniWithServer(t, () => ({ status: 503, body: { message: 'unavailable' } }));
  await assert.rejects(mini.rpc('getUpdateStatus', {}, 30_000), /error checking for updates: could not reach the update server/);
  assert.ok(server.asked.length > 0, 'the Mini asked its update server');
});

test('a check for a version that the update server does not have is refused', { timeout: 120_000 }, async t => {
  const unknown = '0.0.1';
  const { mini, server } = await miniWithServer(t, (query, base) => (query.systemVersion
    ? { status: 404, body: { name: 'NotFoundError', message: 'Release not found' } } : { body: release(base, STABLE) }));
  await assert.rejects(mini.rpc('checkUpdateComponents', { params: { components: { system: unknown } } }, 30_000),
    /requested version was not found on the update server/);
  assert.ok(server.asked.some(({ query }) => query.systemVersion === unknown), 'the Mini asked for that version');
});

// The newest production release of a board, read from the production update
// server: { version, image }.
async function productionRelease(sku) {
  const get = async url => {
    const response = await fetch(url, { signal: AbortSignal.timeout(120_000) });
    if (!response.ok) throw new Error(`${url}: ${response.status}`);
    return response;
  };
  const { systemVersion, systemUrl, systemHash } = await (await get(`${PRODUCTION}/releases?deviceId=jetkvm-integration&sku=${sku}`)).json();
  const image = Buffer.from(await (await get(systemUrl)).arrayBuffer());
  assert.equal(createHash('sha256').update(image).digest('hex'), systemHash.toLowerCase(), `the SHA-256 of ${systemUrl}`);
  return { version: systemVersion, image };
}

// The board that an image is built for: jetkvm_ota_image_desc_t at offset
// 288 of the image, "JKSK" and then the SKU (tools/mini_image.py).
const builtFor = image => (image.subarray(288, 292).toString('latin1') === 'JKSK'
  ? image.subarray(292, 324).toString('utf8').replace(/\0.*$/s, '') : null);

test('an image that is built for the other board is refused', { timeout: 420_000 }, async t => {
  const offer = { version: null, image: Buffer.alloc(0), sha256: '' };
  const { mini, server } = await miniWithServer(t, (query, base) => (query.systemVersion === offer.version
    ? { body: { systemVersion: offer.version, systemUrl: `${base}/other-board.bin`, systemHash: offer.sha256 } }
    : { status: 404, body: { name: 'NotFoundError', message: 'Release not found' } }),
  { get '/other-board.bin'() { return offer.image; } });
  const { sku } = await (await fetch(new URL('/device/status', mini.url), { signal: AbortSignal.timeout(10_000) })).json();
  assert.ok(SKUS.includes(sku), `sku ${sku}`);
  const other = SKUS.find(one => one !== sku);
  Object.assign(offer, await productionRelease(other));
  offer.sha256 = createHash('sha256').update(offer.image).digest('hex');
  assert.equal(builtFor(offer.image), other, 'the production image of the other board names that board');
  // A version that the Mini runs is no update, also when asked for by its number.
  const before = await mini.status();
  if (before.firmware.version.replace(/\+.*$/, '') === offer.version) {
    t.skip(`the Mini runs ${offer.version} already`);
    return;
  }

  // The update check offers no older version, and the other board's release
  // can be older than this build: it is asked for by its number, as "Update
  // to a specific version" of the page does.
  const seen = (await mini.events('otaState')).length;
  await mini.rpc('tryUpdateComponents', { params: { components: { system: offer.version } }, includePreRelease: false, resetConfig: false }, 30_000);
  const states = await until(() => mini.events('otaState'), all => all.slice(seen).some(state => state.error), 120);
  const last = states.at(-1);
  assert.match(last.error, /built for a different board/);
  assert.equal(last.updating, false, 'the update has ended');
  assert.equal(await mini.rpc('isUpdatePending'), false);
  assert.deepEqual({ sku: server.asked.at(-1).query.sku, version: server.asked.at(-1).query.systemVersion }, { sku, version: offer.version },
    'the Mini asked for that version as its own board');
  const after = await mini.status();
  assert.ok(after.uptimeSeconds >= before.uptimeSeconds, 'the Mini did not restart');
  assert.equal(after.firmware.elfSha256, before.firmware.elfSha256);
  assert.equal(after.firmware.version, before.firmware.version);
});

// Session churn fragments memory; the update check must still get its TLS
// connection. Six takeovers by a second page, then a read-only check against
// the production server.
test('the update check reaches the production server over TLS after six other sessions', { timeout: 420_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  const hasVideo = async tab => assert.notEqual(await tab.evaluate(firstFrame, 30_000), null, 'the first frame of video within 30 s');
  const denials = async () => {
    const response = await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(10_000) });
    const { admissionDenials } = (await response.json()).memoryBudget;
    assert.equal(typeof admissionDenials, 'number', 'memoryBudget.admissionDenials of /webrtc/stats');
    return admissionDenials;
  };
  await mini.rpc('setUpdateApiUrl', { url: PRODUCTION });
  await mini.rpc('setDevChannelState', { enabled: false });
  const before = await denials();

  await hasVideo(mini.page);
  for (let session = 0; session < 6; session++) {
    const other = await mini.session();
    try {
      await hasVideo(other.page);
      await new Promise(resolve => setTimeout(resolve, 2000));
    } finally {
      await other.close();
    }
    await mini.open();
    await hasVideo(mini.page);
  }

  const status = await mini.rpc('getUpdateStatus', {}, 30_000);
  assert.ok(status.remote?.systemVersion, `the production server offers a release: ${JSON.stringify(status)}`);
  assert.equal(await denials(), before, 'no memory admission was denied');
});
