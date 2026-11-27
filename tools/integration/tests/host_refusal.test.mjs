import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';
import { gunzipSync } from 'node:zlib';
import { connect } from '../lib/mini.mjs';

const NAME = 'kvm.bench.example';
const BROWSER_ACCEPT = 'text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8';
// The two answers, whole. They are the same for every Mini and every request:
// no address, no name of the Mini, no version and not the name that was sent.
const PAGE = '<!doctype html><html lang="en"><head><meta charset="utf-8">' +
  '<meta name="viewport" content="width=device-width,initial-scale=1">' +
  '<title>This KVM does not answer to this name</title></head><body>' +
  '<h1>This KVM does not answer to this name</h1>' +
  '<p>This JetKVM has no password. For protection, it answers only to its IP address, ' +
  'its hostname, its Tailscale name and the names in its list.</p>' +
  '<p>To use this name:</p>' +
  '<ol><li>Open the KVM by its IP address. The screen of the KVM shows the IP address. ' +
  'If the first setup starts, complete it.</li>' +
  '<li>Go to Settings &gt; Access &gt; Other Names for This KVM.</li>' +
  '<li>Add the name, and save.</li></ol>' +
  '<p>Or set a password in Settings &gt; Access. With a password, each name works.</p>' +
  '</body></html>';
const JSON_ANSWER = { error: 'Unknown host', hint: 'Open the KVM by its IP address and add this name in ' +
  'Settings > Access > Other Names for This KVM, or set a password. The screen of the KVM shows the IP address.' };

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
      // The Mini stores the page of the UI compressed and sends it so.
      response.on('end', () => {
        const bytes = Buffer.concat(chunks);
        resolve({ status: response.statusCode, type: response.headers['content-type'], cache: response.headers['cache-control'],
          body: (response.headers['content-encoding'] === 'gzip' ? gunzipSync(bytes) : bytes).toString() });
      });
    });
    request.on('timeout', () => request.destroy(new Error(`no answer for Host ${host}`)));
    request.on('error', reject);
  });
  let found;
  t.after(async () => {
    try {
      if (found) await mini.rpc('setExtraHostnames', { hostnames: found });
    } finally {
      await mini.close();
    }
  });
  found = await mini.rpc('getExtraHostnames');
  await mini.rpc('setExtraHostnames', { hostnames: [] });
  const refused = (answer, type) => {
    assert.equal(answer.status, 421);
    assert.equal(answer.type, type);
    assert.equal(answer.cache, 'no-cache');
  };

  // A browser that opens the page, or a route of the UI.
  for (const path of ['/', '/settings/access']) {
    const page = await asked(NAME, path, { Accept: BROWSER_ACCEPT });
    refused(page, 'text/html; charset=utf-8');
    assert.equal(page.body, PAGE);
  }
  // A script without an Accept, a fetch, and a file that a page loads.
  for (const [path, headers] of [['/device', {}], ['/device', { Accept: '*/*' }], ['/', { Accept: 'application/json' }]]) {
    const answer = await asked(NAME, path, headers);
    refused(answer, 'application/json');
    assert.deepEqual(JSON.parse(answer.body), JSON_ANSWER);
  }

  // The way that the answer names works: the name in the list is answered.
  assert.deepEqual(await mini.rpc('setExtraHostnames', { hostnames: [NAME] }), [NAME]);
  const page = await asked(NAME, '/', { Accept: BROWSER_ACCEPT });
  assert.equal(page.status, 200);
  assert.match(page.body, /src="\/static\/[^"]+\.js"/, 'the page of the UI');
});
