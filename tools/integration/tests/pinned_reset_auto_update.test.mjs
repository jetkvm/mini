import assert from 'node:assert/strict';
import { test } from 'node:test';
import { beforeErase, eraseBench } from '../lib/erase.mjs';
import { connect, until } from '../lib/mini.mjs';
import { updateServer } from '../lib/servers.mjs';

// The version under which the stand-in update server gives this build to a
// Mini that asks for it by that number.
const ANNOUNCED = '98.0.0';

// The reset would default automatic updates to on, which would replace the
// pinned version. Restores like factory_reset.test.mjs (lib/erase.mjs).
test('a pinned install with a reset of the configuration leaves automatic updates off', { timeout: 900_000 }, async t => {
  const bench = eraseBench();
  if (bench.skip) {
    t.skip(bench.skip);
    return;
  }
  const mini = await connect();
  const setup = async () => (await fetch(new URL('/device/status', mini.url), { signal: AbortSignal.timeout(10_000) })).json();
  const setUp = () => mini.http('/device/setup', { method: 'POST', body: { localAuthMode: 'noPassword' } });
  let server = null;
  let erase = null;
  let erased = false;
  let startedAt = null;
  t.after(async () => {
    try {
      if (!erase) return;
      // After a failure above too: an update that the test started has
      // ended, and the Mini is on its network and set up without a password.
      if (startedAt !== null) {
        await until(async () => (await mini.restarted(startedAt)) || (await mini.rpc('isUpdatePending')) === false, Boolean, 300)
          .catch(() => undefined);
      }
      await until(setup, status => status && !status.factoryResetPending, 300);
      if (!(await setup()).isSetup) assert.equal(await setUp(), 201);
      await mini.open();
      await erase.restore();
    } finally {
      // Also when the Mini does not come back: an open browser or server
      // keeps the test's process alive.
      await mini.close().finally(() => {
        server?.close();
        return erase?.wifiDone(erased);
      });
    }
  });

  // The Mini does not give a stored password out, so the erase would lose it.
  if ((await mini.rpc('getMqttSettings')).password !== '') {
    t.skip('the reset of the configuration erases the Mini\'s MQTT password, which no call of the Mini gives back');
    return;
  }
  erase = await beforeErase(mini, bench);
  await erase.wifiSetup();
  server = await updateServer(mini, { signature: 'none' });
  await mini.rpc('setUpdateApiUrl', { url: server.base });
  // As a user who had them on: the pinned install turns them off.
  await mini.rpc('setAutoUpdateState', { enabled: true });

  startedAt = Date.now();
  erased = true;
  // The Mini can restart before its answer arrives.
  mini.rpc('tryUpdateComponents', { params: { components: { system: ANNOUNCED } }, includePreRelease: false, resetConfig: true }, 30_000)
    .catch(() => undefined);
  await until(setup, status => status?.isSetup === false && !status.factoryResetPending, 600);

  assert.equal(await setUp(), 201);
  await mini.open();
  assert.equal((await mini.status()).firmware.elfSha256, mini.image.elfSha256, 'the Mini runs this build');
  assert.equal(await mini.restarted(startedAt), true);
  assert.equal(await mini.rpc('getAutoUpdateState'), false, 'automatic updates are off after the pinned install and the reset');
  assert.equal(await mini.rpc('getUpdateApiUrl'), 'https://api.jetkvm.com', 'the reset erased the update server');
});
