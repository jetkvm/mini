import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';

const PASSWORD = 'TestPassword123';

// Five wrong passwords give 401, then 429 (login_limit_t in jetkvm_access.c).
// No call clears the lockout, so the test reboots the Mini at the end.
test('the Mini answers a login with 429 after too many wrong passwords', { timeout: 300_000 }, async t => {
  const mini = await connect();
  const origin = new URL(mini.url).origin;
  const send = (method, route, body, cookie) => fetch(origin + route, {
    method,
    headers: { 'Content-Type': 'application/json', Origin: origin, ...(cookie ? { Cookie: cookie } : {}) },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(10_000),
  });
  // Whether the Mini asks for no password: /device answers without a login.
  const open = async () => (await fetch(`${origin}/device`, { signal: AbortSignal.timeout(10_000) })).status === 200;
  // The login that set the password; it takes the password off again.
  let cookie = null;
  t.after(async () => {
    try {
      if (cookie === null) return;
      const removed = await send('DELETE', '/auth/local-password', { password: PASSWORD }, cookie);
      assert.equal(removed.status, 200, 'the Mini takes the password off again');
      assert.equal(await open(), true, 'the Mini asks for no password again');
      await mini.open();
      const startedAt = Date.now();
      // The Mini can restart before its answer arrives.
      mini.rpc('reboot', { force: true }, 5000).catch(() => undefined);
      await until(() => mini.restarted(startedAt), restarted => restarted, 120);
      assert.equal(await open(), true, 'the Mini asks for no password after its restart');
    } finally {
      await mini.close();
    }
  });

  const startedAt = Date.now();
  assert.equal(await open(), true, 'the test needs a Mini without a password');
  const created = await send('POST', '/auth/password-local', { password: PASSWORD });
  assert.equal(created.status, 201, 'the Mini takes a password');
  cookie = (created.headers.get('set-cookie') ?? '').split(';')[0];
  assert.equal(await open(), false, 'the Mini asks for the password');

  let limited = false;
  for (let attempt = 0; attempt < 10; attempt++) {
    const { status } = await send('POST', '/auth/login-local', { password: 'wrongpassword123' });
    if (status === 429) {
      t.diagnostic(`429 after ${attempt} answers with 401`);
      limited = true;
      break;
    }
    assert.equal(status, 401, `wrong password ${attempt + 1}`);
  }
  assert.equal(limited, true, 'ten wrong passwords bring a 429');
  assert.ok(Date.now() - startedAt <= 180_000, 'the test takes 180 seconds at most');
});
