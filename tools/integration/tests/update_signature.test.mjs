import assert from 'node:assert/strict';
import { test } from 'node:test';
import { storedSettings } from '../lib/erase.mjs';
import { connect, until } from '../lib/mini.mjs';
import { mqttBroker, updateServer } from '../lib/servers.mjs';

// esp_reset_reason(): a restart that the firmware asked for, as after an update.
const ESP_RST_SW = 3;
const PRERELEASE = '99.0.0-dev202610130000';

// Stable versions need a release-key signature; dev-channel prereleases and
// pinned versions do not. start(mini) defaults to tryUpdate ("Update Now").

// The Mini, pointed at an update server with this offer, on or off the dev
// channel.
async function miniWithOffer(t, offer, devChannel) {
  const mini = await connect();
  let server;
  const startedAt = Date.now();
  t.after(async () => {
    // An update that the test started ends before the bench is given back.
    await until(async () => (await mini.restarted(startedAt)) || (await mini.rpc('isUpdatePending')) === false, Boolean, 180)
      .catch(() => undefined);
    await mini.close().finally(() => server?.close());
  });
  server = await updateServer(mini, offer);
  await mini.rpc('setUpdateApiUrl', { url: server.base });
  await mini.rpc('setDevChannelState', { enabled: devChannel });
  return { mini, server };
}

// The update is refused with this text and the Mini goes on as it was. It
// is refused before the Mini asks for the image, or, with afterImage, when
// the Mini has taken the whole image.
async function refuses(t, offer, { devChannel = false, error, afterImage = false }) {
  const { mini, server } = await miniWithOffer(t, offer, devChannel);
  const before = await mini.status();
  const seen = (await mini.events('otaState')).length;
  await mini.rpc('tryUpdate', {}, 30_000);
  const states = await until(() => mini.events('otaState'), all => all.slice(seen).some(state => state.error), afterImage ? 180 : 60);
  const last = states.at(-1);
  assert.match(last.error, error);
  assert.equal(last.updating, false, 'the update has ended');
  assert.equal(await mini.rpc('isUpdatePending'), false);
  assert.equal(server.served.firmwareBytes, afterImage ? mini.image.bytes.length : 0,
    afterImage ? 'the Mini took the whole image' : 'the Mini did not ask for the image');
  const after = await mini.status();
  assert.ok(after.uptimeSeconds >= before.uptimeSeconds, 'the Mini did not restart');
  assert.equal(after.firmware.elfSha256, before.firmware.elfSha256);
}

// The Mini takes the whole image, restarts by itself and runs the image.
// prepare(mini, server) runs before the update is asked for. Returns the
// Mini.
async function installs(t, offer, { devChannel = false, start = mini => mini.rpc('tryUpdate', {}, 30_000), prepare = () => undefined } = {}) {
  const { mini, server } = await miniWithOffer(t, offer, devChannel);
  await prepare(mini, server);
  const startedAt = Date.now();
  // The Mini can restart before its answer arrives.
  start(mini).catch(() => undefined);
  await until(() => mini.restarted(startedAt), restarted => restarted, 180);
  assert.equal(server.served.firmwareBytes, mini.image.bytes.length, 'the Mini took the whole image');
  const after = await mini.status();
  assert.equal(after.firmware.elfSha256, mini.image.elfSha256, 'the Mini runs the image after the update');
  assert.equal(after.resetReason, ESP_RST_SW, 'the restart was the firmware\'s');
  return mini;
}

test('a stable version without a signature is refused', { timeout: 120_000 }, t =>
  refuses(t, { signature: 'none' }, { error: /requires GPG signature but API returned no signature URL/ }));

test('a stable version with an empty signature is refused', { timeout: 120_000 }, t =>
  refuses(t, { signature: 'empty' }, { error: /signature file is empty/ }));

test('a stable version with a signature of another key is refused', { timeout: 120_000 }, t =>
  refuses(t, { signature: 'other key' }, { error: /GPG signature verification failed: no signature by the JetKVM release key/ }));

// The signature can only be checked against the whole image, which is in the
// other slot by then; the Mini reports the error and does not restart.
test('a stable version with the release key\'s signature of another image is refused after the download', { timeout: 300_000 }, t =>
  refuses(t, { signature: 'other image' },
    { error: /GPG signature verification failed: the signature does not match the data/, afterImage: true }));

// Update URL, MQTT and NTP servers are set away from defaults so a reset to
// default would show.
test('a stable version with a signature of the release key is installed, and the Mini keeps its settings', { timeout: 300_000 }, async t => {
  let before;
  const mini = await installs(t, { signature: 'valid' }, {
    prepare: async (mini, server) => {
      const broker = await mqttBroker(mini);
      t.after(() => broker.close());
      const mqtt = { enabled: true, broker: broker.host, port: broker.port, base_topic: 'integration-update', use_tls: false };
      await mini.rpc('setMqttSettings', { settings: { ...(await mini.rpc('getMqttSettings')), ...mqtt } }, 30_000);
      const time = { time_sync_mode: 'custom', time_sync_ntp_servers: [new URL(server.base).hostname], time_sync_disable_fallback: false };
      await mini.rpc('setNetworkSettings', { settings: { ...(await mini.rpc('getNetworkSettings')), ...time } });
      await until(() => mini.rpc('getNetworkSettings'), now => now.time_sync_mode === 'custom', 5);
      before = await storedSettings(mini);
      assert.deepEqual({ ...before.mqtt, ...mqtt }, before.mqtt, 'the Mini has stored the MQTT settings');
      assert.deepEqual({ ...before.networkSettings, ...time }, before.networkSettings, 'the Mini has stored the time servers');
    },
  });
  await mini.open();
  assert.deepEqual(await storedSettings(mini), before, 'the stored settings after the update');
  t.diagnostic(`compared: ${Object.keys(before).join(', ')}`);
});

test('a prerelease without a signature is refused off the dev channel', { timeout: 120_000 }, t =>
  refuses(t, { version: PRERELEASE, signature: 'none' }, { error: /requires GPG signature but API returned no signature URL/ }));

test('a prerelease without a signature is installed on the dev channel', { timeout: 300_000 }, t =>
  installs(t, { version: PRERELEASE, signature: 'none' }, { devChannel: true }));

// "Update to a specific version" of the page: the user asks for it at the
// Mini, so the update server cannot start it.
test('a version that the user asks for by its number is installed without a signature', { timeout: 300_000 }, t =>
  installs(t, { signature: 'none' }, {
    start: mini => mini.rpc('tryUpdateComponents',
      { params: { components: { system: '98.0.0' } }, includePreRelease: false, resetConfig: false }, 30_000),
  }));
