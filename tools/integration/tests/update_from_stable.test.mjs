import assert from 'node:assert/strict';
import { test } from 'node:test';
import { beforeErase, eraseBench } from '../lib/erase.mjs';
import { connectHost, hostAgent } from '../lib/host.mjs';
import { spaceReachesHost } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { updateServer } from '../lib/servers.mjs';

const PRODUCTION = 'https://api.jetkvm.com';
// The version under which the stand-in update server gives this build to a
// Mini that asks for it by that number.
const ANNOUNCED = '98.0.0';

// Both installs use pinned versions, which need no signature (the release
// does not trust the test key). Restores like factory_reset.test.mjs.
test('the latest stable release installs this build with a reset of the configuration', { timeout: 1_500_000 }, async t => {
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
  // The Mini can restart before its answer arrives.
  const install = (version, resetConfig) => {
    startedAt = Date.now();
    mini.rpc('tryUpdateComponents', { params: { components: { system: version } }, includePreRelease: false, resetConfig }, 30_000)
      .catch(() => undefined);
  };
  t.after(async () => {
    try {
      if (!erase) return;
      // After a failure above too: an update that the test started has
      // ended, and the Mini is on its network, set up without a password,
      // and runs this build.
      if (startedAt !== null) {
        await until(async () => (await mini.restarted(startedAt)) || (await mini.rpc('isUpdatePending')) === false, Boolean, 300)
          .catch(() => undefined);
      }
      await until(setup, status => status && !status.factoryResetPending, 300);
      if (!(await setup()).isSetup) assert.equal(await setUp(), 201);
      await mini.open();
      if ((await mini.status()).firmware.elfSha256 !== mini.image.elfSha256) {
        server ??= await updateServer(mini, { signature: 'none' });
        await mini.rpc('setUpdateApiUrl', { url: server.base });
        install(ANNOUNCED, false);
        await until(() => mini.restarted(startedAt), Boolean, 300);
        await mini.open();
      }
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

  // The host's key events come from the remote agent, a Linux program.
  const host = bench.JETKVM_REMOTE_HOST && (await connectHost(mini)).system === 'linux' ? await hostAgent(t) : null;
  // The Mini does not give a stored password out, so the erase would lose it.
  if ((await mini.rpc('getMqttSettings')).password !== '') {
    t.skip('the reset of the configuration erases the Mini\'s MQTT password, which no call of the Mini gives back');
    return;
  }
  erase = await beforeErase(mini, bench);

  // This build installs the latest stable release of the production server.
  await mini.rpc('setUpdateApiUrl', { url: PRODUCTION });
  await mini.rpc('setDevChannelState', { enabled: false });
  const stable = (await mini.rpc('getUpdateStatus', {}, 30_000)).remote?.systemVersion;
  assert.ok(stable, 'the production server offers a stable release');
  install(stable, false);
  await until(() => mini.restarted(startedAt), Boolean, 420);
  assert.equal((await mini.status()).firmware.version, stable, 'the Mini runs the stable release');

  // The release installs this build, with a reset of the configuration. A
  // Wi-Fi Mini is on no network after it, until the host has sent the
  // bench's network over BLE.
  await mini.open();
  await erase.wifiSetup(mini.image.version);
  server = await updateServer(mini, { signature: 'none' });
  await mini.rpc('setUpdateApiUrl', { url: server.base });
  erased = true;
  install(ANNOUNCED, true);
  await until(setup, status => status?.isSetup === false && !status.factoryResetPending, 600);

  // The Mini is set up again, without a password, and runs this build.
  assert.equal(await setUp(), 201);
  await mini.open();
  const now = await mini.status();
  assert.equal(now.firmware.version, mini.image.version);
  assert.equal(now.firmware.elfSha256, mini.image.elfSha256, 'the Mini runs this build');
  assert.equal(await mini.restarted(startedAt), true);

  // A key reaches the host, and video is back.
  if (host) assert.ok((await spaceReachesHost(mini, host, 30)).length > 0, 'the host gets a key');
  else t.diagnostic('the key at the host is not checked: the bench has no Linux host with the remote agent');
  await until(async () => (await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(3000) })).json(),
    stats => stats.modeValid === true, 30);
  const frames = (await mini.frames()) ?? 0;
  await until(() => mini.frames(), count => count > frames, 10);
});
