import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect } from '../lib/mini.mjs';

const FIRST = 'TestPassword123';
const SECOND = 'NewPassword456';

// The Mini has one login at a time, so logins go through the page's browser
// (mini.http); `visitor` is a cookie-less client.
test('a password set over HTTP makes the Mini ask for a login, a changed one replaces it, and a removed one opens the Mini again', { timeout: 180_000 }, async t => {
  const mini = await connect();
  const post = (pathname, body) => mini.http(pathname, { method: 'POST', body });
  const visitor = async () => {
    const answer = await fetch(new URL('/device', mini.url), { signal: AbortSignal.timeout(10_000) });
    return { status: answer.status, authMode: (await answer.json().catch(() => null))?.authMode };
  };
  const open = { status: 200, authMode: 'noPassword' };
  // Set before the first call that changes the Mini.
  let changed = false;
  t.after(async () => {
    try {
      // After a failure with a password on the Mini: the browser logs in with
      // the password of the step at which the test ended, and takes it off.
      if (changed && (await visitor()).status !== 200) {
        const password = await post('/auth/login-local', { password: SECOND }) === 200 ? SECOND : FIRST;
        assert.equal(await post('/auth/login-local', { password }), 200);
        assert.equal(await mini.http('/auth/local-password', { method: 'DELETE', body: { password } }), 200, 'the test\'s password is removed again');
      }
    } finally {
      await mini.close();
    }
  });
  // The browser has no login, and gets in with `password` alone.
  const loginTakes = async (password, other) => {
    assert.equal(await post('/auth/logout'), 200);
    assert.equal(await mini.http('/device'), 401, 'the Mini asks the browser for a login');
    if (other) assert.equal(await post('/auth/login-local', { password: other }), 401, 'the replaced password is not the password');
    assert.equal(await post('/auth/login-local', { password }), 200);
    assert.equal(await mini.http('/device'), 200);
  };

  assert.deepEqual(await visitor(), open, 'the bench\'s Mini has no password');

  changed = true;
  assert.equal(await post('/auth/password-local', { password: FIRST }), 201, 'the Mini takes a password');
  assert.equal((await visitor()).status, 401, 'the Mini asks a visitor for a login');
  assert.equal(await mini.http('/device'), 200, 'the browser that set the password is logged in');
  await loginTakes(FIRST);

  assert.equal(await mini.http('/auth/password-local', { method: 'PUT', body: { oldPassword: FIRST, newPassword: SECOND } }), 200, 'the Mini takes another password for the first');
  assert.equal((await visitor()).status, 401);
  assert.equal(await mini.http('/device'), 200, 'the browser that changed the password is logged in');
  await loginTakes(SECOND, FIRST);

  assert.equal(await mini.http('/auth/local-password', { method: 'DELETE', body: { password: SECOND } }), 200, 'the Mini gives its password up for the password');
  assert.deepEqual(await visitor(), open, 'the Mini asks nobody for a login');
  assert.equal(await post('/auth/login-local', { password: SECOND }), 400, 'there is no password to log in with');
});
