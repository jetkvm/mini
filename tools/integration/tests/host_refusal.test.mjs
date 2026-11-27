import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';
import { connect } from '../lib/mini.mjs';

const NAME = 'kvm.bench.example';
const BROWSER_ACCEPT = 'text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8';

// The 421 of the DNS rebinding guard says how to make a name work. A page
// under a foreign name can read that answer, so it names nothing of the Mini.
test('a name the Mini does not know gets 421 with the way to add it, and no detail of the Mini', { timeout: 120_000 }, async t => {
  const mini = await connect();
  const url = new URL(mini.url);
  const asked = (host, path, headers = {}) => new Promise((resolve, reject) => {
    const request = http.get({ host: url.hostname, port: url.port || 80, path, agent: false, timeout: 10_000,
      headers: { Host: host, ...headers } }, response => {
      const chunks = [];
      response.on('data', chunk => chunks.push(chunk));
      response.on('end', () => resolve({ status: response.statusCode, type: response.headers['content-type'],
        cache: response.headers['cache-control'], body: Buffer.concat(chunks).toString() }));
    });
    request.on('timeout', () => request.destroy(new Error(`no answer for Host ${host}`)));
    request.on('error', reject);
  });
  const found = await mini.rpc('getExtraHostnames');
  t.after(async () => {
    try {
      await mini.rpc('setExtraHostnames', { hostnames: found });
    } finally {
      await mini.close();
    }
  });
  await mini.rpc('setExtraHostnames', { hostnames: [] });
  const { hostname } = await mini.rpc('getNetworkSettings');
  const secrets = [url.hostname, hostname, NAME, await mini.rpc('getDeviceID'), (await mini.status()).firmware.version];
  const refused = (answer, type) => {
    assert.equal(answer.status, 421);
    assert.equal(answer.type, type);
    assert.equal(answer.cache, 'no-cache');
    assert.match(answer.body, /Other Names for This KVM/);
    assert.match(answer.body, /The screen of the KVM shows the IP address\./);
    for (const secret of secrets) assert.ok(!answer.body.includes(secret), `the answer has "${secret}"`);
    // Any IPv4 address, not only the one of this connection.
    assert.doesNotMatch(answer.body, /\d+\.\d+\.\d+\.\d+/);
  };

  // A browser that opens the page, or a route of the UI.
  for (const path of ['/', '/settings/access']) {
    const page = await asked(NAME, path, { Accept: BROWSER_ACCEPT });
    refused(page, 'text/html; charset=utf-8');
    assert.match(page.body, /^<!doctype html>/);
    assert.doesNotMatch(page.body, /<script|href=|src=/);
  }
  // A script without an Accept, a fetch, and a file that a page loads.
  for (const [path, headers] of [['/device', {}], ['/device', { Accept: '*/*' }], ['/', { Accept: 'application/json' }]]) {
    const answer = await asked(NAME, path, headers);
    refused(answer, 'application/json');
    const json = JSON.parse(answer.body);
    assert.equal(json.error, 'Unknown host');
    assert.equal(typeof json.hint, 'string');
  }

  // The way that the answer names works: the name in the list is answered.
  assert.deepEqual(await mini.rpc('setExtraHostnames', { hostnames: [NAME] }), [NAME]);
  const page = await asked(NAME, '/', { Accept: BROWSER_ACCEPT });
  assert.equal(page.status, 200);
  assert.match(page.body, /src="\/static\/[^"]+\.js"/, 'the page of the UI');
});
