import assert from 'node:assert/strict';
import { test } from 'node:test';
import { beforeErase, eraseBench } from '../lib/erase.mjs';
import { connect, until } from '../lib/mini.mjs';
import { DEFAULT_CONTROL_URL, KEY_FILE, cleanState, config, join, status } from '../lib/tailscale.mjs';

// A password has 8 to 72 characters (components/jetkvm_access/password_core.c).
const TOO_SHORT = 'p'.repeat(7);
const TOO_LONG = 'p'.repeat(73);
const OLD_PASSWORD = 'o'.repeat(8);
const NEW_PASSWORD = 'n'.repeat(72);

// One reset covers the password, setup and Tailscale checks.
test('a factory reset erases the password and the Tailscale node, and the setup takes a password of 8 to 72 characters only', { timeout: 600_000 }, async t => {
  const bench = eraseBench();
  if (bench.skip) {
    t.skip(bench.skip);
    return;
  }
  const mini = await connect();
  // What a visitor who has not logged in gets. The calls of mini.http are
  // those of a browser that keeps its login.
  const visitor = async pathname => {
    const answer = await fetch(new URL(pathname, mini.url), { signal: AbortSignal.timeout(10_000) });
    return { status: answer.status, body: await answer.json().catch(() => null) };
  };
  const open = async () => (await visitor('/device')).status === 200;
  const setup = async () => (await visitor('/device/status')).body;
  const post = (pathname, body) => mini.http(pathname, { method: 'POST', body });
  const removePassword = password => mini.http('/auth/local-password', { method: 'DELETE', body: { password } });

  let erase = null;
  // Set before the first call that changes the Mini, and before the reset.
  let changed = false;
  let wasReset = false;
  t.after(async () => {
    try {
      if (!changed) return;
      // After a failure above too: the Mini is on its network, set up, and
      // without a password, as every bench is.
      await until(setup, status => status && !status.factoryResetPending, 180);
      if (!(await setup()).isSetup) assert.equal(await post('/device/setup', { localAuthMode: 'noPassword' }), 201);
      // The Mini has the password of the step at which the test ended, and
      // takes it off for a browser that has logged in with it.
      if (!await open()) {
        const password = await post('/auth/login-local', { password: NEW_PASSWORD }) === 200 ? NEW_PASSWORD : OLD_PASSWORD;
        assert.equal(await post('/auth/login-local', { password }), 200);
        assert.equal(await removePassword(password), 200, 'the test\'s password is removed again');
      }
      // Every setting as found, and the sender's approval of the Mini.
      await mini.open();
      await erase.restore();
      await cleanState(mini);
    } finally {
      // Also when the Mini does not come back: an open browser keeps the
      // test's process alive.
      await mini.close().finally(() => erase?.wifiDone(wasReset));
    }
  });

  // Before the reset: what the reset erases, and that nothing is in use that
  // it cannot give back.
  assert.equal(await open(), true, 'the bench\'s Mini has no password');
  // The Mini does not give a stored password out, so the reset would lose it.
  assert.equal((await mini.rpc('getMqttSettings')).password, '', 'the Mini has no MQTT password');
  erase = await beforeErase(mini, bench);
  await erase.wifiSetup();
  // A node on the tailnet before the reset, with a real auth key when the
  // bench has one: the reset must erase its identity, so that the Mini does
  // not come back as that node.
  changed = true;
  await cleanState(mini);
  if (KEY_FILE) await join(mini);

  // A new password on a Mini without one follows the length rule.
  assert.equal(await post('/auth/password-local', { password: TOO_SHORT }), 400, '7 characters are refused');
  assert.equal(await post('/auth/password-local', { password: TOO_LONG }), 400, '73 characters are refused');
  assert.equal(await open(), true, 'a refused password protects nothing');
  assert.equal(await post('/auth/password-local', { password: OLD_PASSWORD }), 201, '8 characters are taken');
  assert.equal(await open(), false, 'the Mini asks a visitor for the password');

  // The reset, by RPC, from a page that has logged in with the password.
  assert.equal(await post('/auth/logout'), 200);
  assert.equal(await post('/auth/login-local', { password: OLD_PASSWORD }), 200);
  await mini.open();
  wasReset = true;
  await mini.rpc('factoryReset');
  // The Mini restarts, erases its settings, and answers again: as not set up.
  const erased = await until(setup, status => status?.isSetup === false && !status.factoryResetPending, 120);
  assert.equal(erased.factoryResetError, undefined);
  // Before the setup the Mini answers its setup only: no API call, no
  // signaling and no password change.
  assert.deepEqual(await visitor('/device'), { status: 401, body: { error: 'Setup required' } });
  assert.equal(await post('/webrtc/session', {}), 401, 'no session before the setup');
  assert.equal(await post('/auth/password-local', { password: OLD_PASSWORD }), 401, 'no password before the setup');
  assert.equal(await post('/auth/login-local', { password: OLD_PASSWORD }), 400, 'there is no password to log in with');

  // The setup follows the length rule, and a refused setup is no setup.
  assert.equal(await post('/device/setup', { localAuthMode: 'password', password: TOO_SHORT }), 400, '7 characters are refused');
  assert.equal(await post('/device/setup', { localAuthMode: 'password', password: TOO_LONG }), 400, '73 characters are refused');
  assert.equal((await setup()).isSetup, false);
  assert.equal(await post('/device/setup', { localAuthMode: 'password', password: NEW_PASSWORD }), 201, '72 characters are taken');
  assert.equal((await setup()).isSetup, true);

  // The password of the setup is the Mini's password: a visitor is refused,
  // the browser that made the setup is logged in, and after a logout it gets
  // in with the password only.
  assert.equal(await open(), false);
  assert.equal(await mini.http('/device'), 200);
  assert.equal(await post('/auth/logout'), 200);
  assert.equal(await mini.http('/device'), 401);
  assert.equal(await post('/auth/login-local', { password: OLD_PASSWORD }), 401, 'the old password is not the password');
  assert.equal(await post('/auth/login-local', { password: NEW_PASSWORD }), 200);
  assert.equal(await mini.http('/device'), 200);

  // The page of the set-up Mini has an RPC session, and the Mini gives its
  // password up for the password: the state of every bench.
  await mini.open();
  assert.equal(await removePassword(NEW_PASSWORD), 200);
  assert.equal(await open(), true);
  assert.equal((await setup()).isSetup, true);

  // Tailscale is off with no key and the default control server, and the
  // node's identity is gone: enabled without a key, the Mini asks for a
  // login in place of coming back as the node from before the reset.
  assert.deepEqual(await config(mini), { enabled: false, authKeySet: false, controlURL: DEFAULT_CONTROL_URL });
  assert.equal((await status(mini)).backendState, 'Stopped');
  await mini.rpc('setTailscaleConfig', { enabled: true });
  const login = await until(() => status(mini), now => now.backendState === 'NeedsLogin' || now.running, 100);
  assert.equal(login.running, false, 'the Mini does not join as the node from before the reset');
  assert.match(login.authURL ?? '', /^https:\/\/login\.tailscale\.com\//, 'the status has a login link');
  assert.equal(login.self, undefined);
  await mini.rpc('setTailscaleConfig', { enabled: false });
  const stopped = await until(() => status(mini), now => now.backendState === 'Stopped', 40);
  assert.equal(stopped.authURL, undefined, 'a stopped client has no login link');
});
