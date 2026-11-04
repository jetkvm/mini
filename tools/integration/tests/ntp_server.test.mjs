import assert from 'node:assert/strict';
import { lookup } from 'node:dns/promises';
import { test } from 'node:test';
import { connectHost, ntpResponder } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';

// The bench's host is the one NTP server of the Mini, without the public
// servers, and counts the Mini's requests. The NTP server needs a Linux host.
test('the Mini asks a custom NTP server when the server is stored', { timeout: 240_000 }, async t => {
  const mini = await connect();
  let ntp;
  // close() stores the network settings as connect() found them.
  t.after(() => mini.close().finally(() => ntp?.stop()));
  const host = await connectHost(mini);
  if (host.system !== 'linux') return void t.skip(`the bench's host is ${host.system}: this needs a Linux host`);
  ntp = await ntpResponder(host, (await lookup(new URL(mini.url).hostname, { family: 4 })).address);

  // From another configuration, so that storing the server starts a new
  // request also on a Mini that had this server before.
  const other = {
    ...(await mini.rpc('getNetworkSettings')),
    time_sync_mode: 'ntp_only',
    time_sync_ntp_servers: [],
    time_sync_http_urls: [],
    time_sync_disable_fallback: true,
  };
  await mini.rpc('setNetworkSettings', { settings: other });
  // The Mini applies stored network settings shortly after its answer.
  await new Promise(resolve => setTimeout(resolve, 2000));

  const time = { time_sync_mode: 'custom', time_sync_ntp_servers: [host.login.split('@').at(-1)], time_sync_disable_fallback: true };
  await mini.rpc('setNetworkSettings', { settings: { ...other, ...time } });
  await until(() => mini.rpc('getNetworkSettings'), ({ time_sync_mode, time_sync_ntp_servers, time_sync_disable_fallback }) =>
    assert.deepEqual({ time_sync_mode, time_sync_ntp_servers, time_sync_disable_fallback }, time) ?? true, 5);
  await until(() => ntp.count(), count => count > 0, 90);
});
