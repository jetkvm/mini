import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';
import { updateServer } from '../lib/servers.mjs';

// Pre-release check against api.jetkvm.com and the keyservers. Needs a
// signature (systemSigUrl) for the latest stable release. Leaves the Mini on
// that release if the second half fails.
test('the build installs the latest production release, and that release installs the build', {
  timeout: 600_000,
}, async t => {
  const mini = await connect();
  let server;
  t.after(() => mini.close().finally(() => server?.close()));

  await mini.rpc('setUpdateApiUrl', { url: '' });
  await mini.rpc('setDevChannelState', { enabled: false });
  const offer = await mini.rpc('getUpdateStatus', {}, 30_000);
  assert.ok(offer.systemUpdateAvailable, `the cloud offers no update: ${JSON.stringify(offer)}`);
  const release = offer.remote.systemVersion;

  let startedAt = Date.now();
  const seen = (await mini.events('otaState')).length;
  mini.rpc('tryUpdate', {}, 30_000).catch(() => undefined);
  // The update ends with a restart or with an error.
  const refusal = async () => (await mini.events('otaState').catch(() => [])).slice(seen).find(state => state.error)?.error;
  const end = await until(async () => (await refusal()) ?? (await mini.restarted(startedAt).catch(() => false)), Boolean, 300);
  assert.equal(end, true, `the Mini refused the production release ${release}: ${end}`);
  assert.equal((await mini.status()).firmware.version, release, 'the Mini runs the production release');

  // The release does not trust the test key; a pinned version needs no signature.
  server = await updateServer(mini, { signature: 'none' });
  await mini.open();
  await mini.rpc('setUpdateApiUrl', { url: server.base });
  startedAt = Date.now();
  mini.rpc('tryUpdateComponents',
    { params: { components: { system: '98.0.0' } }, includePreRelease: false, resetConfig: false }, 30_000).catch(() => undefined);
  await until(() => mini.restarted(startedAt), restarted => restarted, 300);
  assert.equal((await mini.status()).firmware.elfSha256, mini.image.elfSha256, 'the Mini runs this build again');
});
