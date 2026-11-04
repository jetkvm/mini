// Stand-in JetKVM Cloud (tools/combined_load/runner/fake_cloud.mjs) and adoption.
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { benchFile, readBench } from '../../combined_load/runner/build.mjs';
import { startFakeCloud } from '../../combined_load/runner/fake_cloud.mjs';
import { until } from './mini.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');

/**
 * Starts the stand-in cloud at the bench's JETKVM_MOCK_CLOUD_JWKS_URL. Share
 * one per test file: the Mini refetches a key set at most every 30 s
 * (minimum_refresh_interval_s in components/jetkvm_oidc).
 */
export function standInCloud() {
  const keys = cloudKeysUrl();
  assert.ok(keys, 'the bench file sets JETKVM_MOCK_CLOUD_JWKS_URL');
  return startFakeCloud(keys);
}

// Undefined on a bench without the mock cloud; such tests skip.
export function cloudKeysUrl() {
  return readBench(benchFile(root, process.env.INTEGRATION_BENCH)).JETKVM_MOCK_CLOUD_JWKS_URL || undefined;
}

/**
 * Adoption of an unadopted Mini to `cloud`: { register, adopt, leave }.
 * register() does what the /adopt page does (POST /cloud/register) with the
 * page closed; adopt() also waits for the WebSocket; leave() restores.
 */
export async function adoption(mini, cloud) {
  const { version } = (await mini.status()).firmware;
  assert.match(version, /mockcloud/, 'the Mini runs a build that takes the keys of a login from the stand-in cloud');
  const found = await mini.rpc('getCloudState');
  assert.equal(found.connected, false, `the bench's Mini is not adopted (it is, to ${found.url})`);
  const here = { apiUrl: cloud.url, appUrl: cloud.url };
  const origin = new URL(mini.url).origin;

  const register = async () => {
    await mini.rpc('setCloudUrl', here);
    // As in the UI, registration happens without a session.
    await mini.page.goto('about:blank');
    try {
      // Up to 10 s for the clock plus two TLS handshakes.
      const answer = await fetch(`${origin}/cloud/register`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Origin: origin },
        body: JSON.stringify(cloud.adoption()),
        signal: AbortSignal.timeout(30_000),
      });
      return { status: answer.status, body: await answer.json().catch(() => null) };
    } finally {
      await mini.open();
    }
  };
  const adopt = async () => {
    const { sockets } = cloud.asked();
    const answer = await register();
    assert.equal(answer.status, 200, `the Mini takes the adoption (${JSON.stringify(answer.body)}; ${cloud.problem() ?? 'the cloud saw no problem'})`);
    await until(() => cloud.connected() && cloud.asked().sockets > sockets, Boolean, 30);
  };
  const leave = async () => {
    await mini.open();
    if ((await mini.rpc('getCloudState')).connected) {
      await mini.rpc('setCloudUrl', here);
      await mini.rpc('deregisterDevice', {}, 30_000);
    }
    await mini.rpc('setCloudUrl', { apiUrl: found.url, appUrl: found.appUrl });
    assert.deepEqual(await mini.rpc('getCloudState'), found, 'the Mini\'s cloud state is as found');
  };
  return { register, adopt, leave };
}
