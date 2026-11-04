import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';
import { connect } from '../lib/mini.mjs';

const PASSWORD = 'HostNames123';

// DNS rebinding guard: without a password, an unknown Host gets 421. With a
// password, any Host is accepted (no cookie means 401 anyway).
test('without a password the Mini answers only its own names and the other names given to it, and with a password any name', { timeout: 180_000 }, async t => {
  const mini = await connect();
  const url = new URL(mini.url);
  const asked = (host, { cookie, path = '/device', headers = {} } = {}) => new Promise((resolve, reject) => {
    const request = http.get({ host: url.hostname, port: url.port || 80, path, agent: false, timeout: 10_000,
      headers: { Host: host, ...(cookie ? { Cookie: cookie } : {}), ...headers } }, response => {
      response.resume();
      response.on('end', () => resolve(response.statusCode));
    });
    request.on('upgrade', (response, socket) => { socket.destroy(); resolve(response.statusCode); });
    request.on('timeout', () => request.destroy(new Error(`no answer for Host ${host}`)));
    request.on('error', reject);
  });
  // Every route checks the Host: a guarded one, the page, a file of the UI,
  // a route of the UI and the signaling WebSocket. The WebSocket's Origin is
  // another site's, so a Host that the Mini answers gets 403 there and opens
  // no socket that would take the page's session.
  const asset = (await (await fetch(mini.url)).text()).match(/src="(\/static\/[^"]+\.js)"/)?.[1];
  assert.ok(asset, 'the page names a script of the UI');
  const routes = async host => ({
    device: await asked(host),
    page: await asked(host, { path: '/' }),
    asset: await asked(host, { path: asset }),
    spa: await asked(host, { path: '/settings/access', headers: { Accept: 'text/html' } }),
    signaling: await asked(host, { path: '/webrtc/signaling/client', headers: {
      Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Version': '13',
      'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ==', Origin: 'http://other.bench.example' } }),
  });
  const ANSWERED = { device: 200, page: 200, asset: 200, spa: 200, signaling: 403 };
  const REFUSED = { device: 421, page: 421, asset: 421, spa: 421, signaling: 421 };
  const found = await mini.rpc('getExtraHostnames');
  let password = false;
  t.after(async () => {
    try {
      if (password) {
        assert.equal(await mini.http('/auth/login-local', { method: 'POST', body: { password: PASSWORD } }), 200);
        assert.equal(await mini.http('/auth/local-password', { method: 'DELETE', body: { password: PASSWORD } }), 200);
      }
      await mini.rpc('setExtraHostnames', { hostnames: found });
    } finally {
      await mini.close();
    }
  });
  const { hostname } = await mini.rpc('getNetworkSettings');
  await mini.rpc('setExtraHostnames', { hostnames: [] });

  assert.deepEqual(await routes(url.host), ANSWERED, 'the address');
  assert.equal(await asked(hostname), 200, 'the hostname');
  assert.equal(await asked(`${hostname.toUpperCase()}.local`), 200, 'the hostname with .local');
  assert.deepEqual(await routes('kvm.bench.example'), REFUSED, 'a name the Mini was not given');

  assert.deepEqual(await mini.rpc('setExtraHostnames', { hostnames: ['KVM.Bench.Example.', 'rack-3'] }), ['kvm.bench.example', 'rack-3']);
  assert.deepEqual(await routes('kvm.bench.example'), ANSWERED, 'a name the Mini was given');
  assert.equal(await asked('rack-3:80'), 200, 'a name the Mini was given, with a port');
  assert.equal(await asked('other.bench.example'), 421);
  await assert.rejects(mini.rpc('setExtraHostnames', { hostnames: ['rack-3', 'bad_name'] }), /Invalid host names: A name has letters/);
  assert.deepEqual(await mini.rpc('getExtraHostnames'), ['kvm.bench.example', 'rack-3'], 'a refused list changes nothing');

  assert.deepEqual(await mini.rpc('setExtraHostnames', { hostnames: ['rack-3'] }), ['rack-3']);
  assert.deepEqual(await routes('kvm.bench.example'), REFUSED, 'a name taken away again');
  assert.equal(await asked('rack-3'), 200, 'the name that stays');

  password = true;
  assert.equal(await mini.http('/auth/password-local', { method: 'POST', body: { password: PASSWORD } }), 201);
  const cookie = (await mini.page.context().cookies(mini.url)).find(entry => entry.name === 'authToken');
  assert.ok(cookie, 'the browser that set the password has the login cookie');
  assert.equal(await asked('other.bench.example'), 401, 'with a password a foreign name gets the login question');
  assert.equal(await asked('other.bench.example', { cookie: `authToken=${cookie.value}` }), 200, 'and the login is answered under any name');
  assert.equal(await mini.http('/auth/local-password', { method: 'DELETE', body: { password: PASSWORD } }), 200);
  password = false;
  assert.equal(await asked('other.bench.example'), 421, 'without the password a foreign name gets 421 again');
});
