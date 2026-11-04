import assert from 'node:assert/strict';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { benchFile, readBench } from '../../combined_load/runner/build.mjs';
import { connect, until } from '../lib/mini.mjs';
import { webrtcStats } from '../lib/video.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const bench = readBench(benchFile(root, process.env.INTEGRATION_BENCH ?? ''));
const skip = bench.JETKVM_SKU === 'jetkvm-mini-wireless' ? false : 'the Mini has no Wi-Fi';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// What the local display's Wi-Fi status screen showed: the last reason of a
// failed join that it put up, and how often it put one up.
async function display(mini) {
  const { localDisplay } = await webrtcStats(mini);
  return { shown: localDisplay.wifiFailureShown, shows: localDisplay.wifiFailureShows };
}

// The Mini leaves its network for the request: the page's session ends. The
// Wi-Fi settings once the request is done and the Mini is back online.
async function backOnline(mini, seconds) {
  await sleep(5000);
  return until(async () => {
    await mini.open();
    return mini.rpc('getWifiSettings');
  }, settings => !settings.applying && settings.connected, seconds);
}

// The Mini tests a new password for its own network by joining with it, and
// keeps the stored one when that join fails, so the bench's network stays.
test('a wrong password for the Mini\'s network is refused, and the display shows "Wrong password" and the network', { timeout: 300_000, skip }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  const before = await mini.rpc('getWifiSettings');
  assert.ok(before.connected && before.ssid, `the Mini is on a Wi-Fi network: ${JSON.stringify(before)}`);
  const found = await display(mini);

  const reply = await mini.rpc('saveWifiNetwork', { ssid: before.ssid, password: 'not-the-password-0', connect: true });
  assert.equal(reply.connecting, true);
  const after = await backOnline(mini, 180);
  assert.equal(after.connect_error, 'Wrong password');
  assert.equal(after.ssid, before.ssid, 'the Mini is back on its network with the stored password');
  const now = await display(mini);
  t.diagnostic(`display: ${JSON.stringify(now)}`);
  assert.ok(now.shows > found.shows, 'the display put up the reason');
  assert.equal(now.shown, `Wrong password\n${before.ssid}`);
});

// The Mini takes the other band, finds no saved network there and goes back
// to the band it had. It is a test only where the bench's network is the one
// saved network and is on one band.
test('a saved network that the Mini does not find is named on the display with "Network not found"', { timeout: 300_000, skip }, async t => {
  const mini = await connect();
  // The band as found, once the test may change it.
  let original = null;
  t.after(async () => {
    try {
      // Only when the network is on the other band too: the Mini then keeps it.
      const settings = original && await until(() => mini.rpc('getWifiSettings'), () => true, 60);
      if (settings && settings.band !== original) {
        await mini.rpc('setWifiBand', { band: original });
        await backOnline(mini, 180);
      }
    } finally {
      await mini.close();
    }
  });
  const before = await mini.rpc('getWifiSettings');
  assert.ok(before.connected && before.channel > 0, `the Mini is on a Wi-Fi network: ${JSON.stringify(before)}`);
  if (before.networks.length !== 1 || before.band !== 'auto') {
    t.skip(`needs one saved network and the band auto: ${JSON.stringify(before)}`);
    return;
  }
  const band = before.channel > 14 ? '2.4ghz' : '5ghz';
  const found = await display(mini);
  original = before.band;

  await mini.rpc('setWifiBand', { band });
  const after = await backOnline(mini, 240);
  if (after.band === band) {
    t.skip(`${before.ssid} is on ${band} too`);
    return;
  }
  assert.equal(after.apply_failed, true, 'the band change failed');
  assert.equal(after.band, before.band, 'the Mini went back to its band');
  const now = await display(mini);
  t.diagnostic(`display: ${JSON.stringify(now)}`);
  assert.ok(now.shows > found.shows, 'the display put up the reason');
  assert.equal(now.shown, `Network not found\n${before.ssid}`);
});
