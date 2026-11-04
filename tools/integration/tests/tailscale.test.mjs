import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';
import { DEFAULT_CONTROL_URL, KEY_FILE, NO_KEY, authKey, cleanState, config, join, status, tailnetAddress, tunnelHttp } from '../lib/tailscale.mjs';

test('the Mini reports Tailscale, and refuses invalid settings without changing the stored ones', { timeout: 60_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  assert.ok((await mini.rpc('getDeviceCapabilities')).includes('tailscale'));
  const original = await config(mini);
  const now = await status(mini);
  assert.equal(now.installed, true);
  assert.equal(now.configurable, true);
  assert.equal(await mini.rpc('getTailscaleControlURL'), original.controlURL);
  for (const controlURL of ['ftp://example.com', 'http://headscale.example:8080', 'https://user@example.com', 'https://example.com/path',
    'https://example.com?key=x', 'https://example.com:65536']) {
    await assert.rejects(mini.rpc('setTailscaleControlURL', { controlURL }), `${controlURL} is refused`);
    assert.deepEqual(await config(mini), original);
  }
  for (const params of [{ enabled: 'yes' }, { enabled: true, authKey: 123 },
    { enabled: true, authKey: 'tskey-api-wrong-type' }, { enabled: false, unexpected: true }]) {
    await assert.rejects(mini.rpc('setTailscaleConfig', params), `${JSON.stringify(params)} is refused`);
    assert.deepEqual(await config(mini), original);
  }
  assert.equal('authKey' in original, false, 'the settings do not give a key out');
  assert.equal('authKey' in now, false, 'the status does not give a key out');
});

test('a control server is stored without its trailing slash and survives a restart, without the key of the server before it', { timeout: 240_000 }, async t => {
  const mini = await connect();
  t.after(async () => {
    try {
      await mini.rpc('setTailscaleControlURL', { controlURL: DEFAULT_CONTROL_URL });
    } finally {
      await mini.close();
    }
  });
  await cleanState(mini);
  await mini.rpc('setTailscaleConfig', { enabled: false, authKey: 'tskey-auth-integration-placeholder' });
  assert.equal((await config(mini)).authKeySet, true);
  await mini.rpc('setTailscaleControlURL', { controlURL: 'https://headscale.invalid:8443/' });
  assert.deepEqual(await config(mini), { enabled: false, authKeySet: false, controlURL: 'https://headscale.invalid:8443' });
  const restartAt = Date.now();
  mini.rpc('reboot', { force: true }, 5000).catch(() => undefined);
  await until(() => mini.restarted(restartAt), Boolean, 120);
  await mini.open();
  assert.equal((await config(mini)).controlURL, 'https://headscale.invalid:8443');
  assert.equal(await mini.rpc('getTailscaleControlURL'), 'https://headscale.invalid:8443');
});

test('with an auth key the Mini joins, answers over the tunnel, and keeps its identity across a restart', { timeout: 420_000 }, async t => {
  if (!KEY_FILE) {
    t.skip(NO_KEY);
    return;
  }
  const mini = await connect();
  t.after(async () => {
    try {
      await cleanState(mini);
    } finally {
      await mini.close();
    }
  });
  await cleanState(mini);
  const joined = await join(mini);
  const address = tailnetAddress(joined);
  assert.match(address, /^100\./);
  assert.ok(joined.self.dnsName, 'the node has a DNS name');
  const stored = await config(mini);
  assert.deepEqual(stored, { enabled: true, authKeySet: true, controlURL: DEFAULT_CONTROL_URL });
  const key = authKey();
  for (const answer of [joined, stored, await mini.rpc('getTailscaleControlURL')]) {
    assert.equal(JSON.stringify(answer).includes(key), false, 'no answer has the auth key');
  }
  assert.equal(await tunnelHttp(address, 30_000), 200, 'HTTP reaches the Mini through the tunnel');
  // The settings worker writes the node's identity to flash from an internal stack.
  const details = await (await fetch(new URL('/webrtc/stats?details=1', mini.url), { signal: AbortSignal.timeout(10_000) })).json();
  const worker = details.taskStacks.find(task => task.name === 'jk_tailscale');
  assert.ok(worker, 'the stats have the stack of the Tailscale settings worker');
  assert.ok(worker.highWatermarkBytes >= 1024, `the worker had ${worker.highWatermarkBytes} bytes of its stack left at least`);

  const restartAt = Date.now();
  mini.rpc('reboot', { force: true }, 5000).catch(() => undefined);
  await until(() => mini.restarted(restartAt), Boolean, 120);
  await mini.open();
  const rejoined = await until(() => status(mini), now => now.running && now.self?.online, 120);
  assert.deepEqual(rejoined.self.tailscaleIPs, joined.self.tailscaleIPs, 'the same tailnet address after the restart');
  assert.equal(rejoined.self.dnsName, joined.self.dnsName, 'the same DNS name after the restart');
  assert.equal(await tunnelHttp(address, 30_000), 200);
});

test('disabling Tailscale closes the tunnel, and enabling it brings the same node back, three times', { timeout: 600_000 }, async t => {
  if (!KEY_FILE) {
    t.skip(NO_KEY);
    return;
  }
  const mini = await connect();
  t.after(async () => {
    try {
      await cleanState(mini);
    } finally {
      await mini.close();
    }
  });
  await cleanState(mini);
  const joined = await join(mini);
  const address = tailnetAddress(joined);
  for (let round = 1; round <= 3; round++) {
    await mini.rpc('setTailscaleConfig', { enabled: false });
    await until(() => status(mini), now => now.backendState === 'Stopped' && !now.self, 40);
    assert.notEqual(await tunnelHttp(address, 3000), 200, `round ${round}: a stopped node does not answer over the tunnel`);
    assert.equal((await config(mini)).authKeySet, true, `round ${round}: the key stays stored while Tailscale is off`);
    await mini.rpc('setTailscaleConfig', { enabled: true });
    const back = await until(() => status(mini), now => now.running && now.self?.online, 90);
    assert.deepEqual(back.self.tailscaleIPs, joined.self.tailscaleIPs, `round ${round}: the same tailnet address`);
    assert.equal(back.self.dnsName, joined.self.dnsName, `round ${round}: the same DNS name`);
    assert.equal(await until(() => tunnelHttp(address, 5000), code => code === 200, 30), 200);
  }
  // A control server saved while the client stops does not leave the old
  // node's address in the status, and it erases the key of the server before.
  await mini.rpc('setTailscaleConfig', { enabled: false });
  await mini.rpc('setTailscaleControlURL', { controlURL: 'https://shutdown-race.invalid' });
  await mini.rpc('setTailscaleControlURL', { controlURL: DEFAULT_CONTROL_URL });
  await until(() => status(mini), now => now.backendState === 'Stopped' && !now.self, 40);
  assert.equal((await config(mini)).authKeySet, false, 'a new control server erases the stored key');
});

test('removing the auth key disables Tailscale and closes the tunnel', { timeout: 300_000 }, async t => {
  if (!KEY_FILE) {
    t.skip(NO_KEY);
    return;
  }
  const mini = await connect();
  t.after(async () => {
    try {
      await cleanState(mini);
    } finally {
      await mini.close();
    }
  });
  await cleanState(mini);
  const address = tailnetAddress(await join(mini));
  await mini.rpc('setTailscaleConfig', { enabled: false, authKey: '' });
  const stopped = await until(() => status(mini), now => now.backendState === 'Stopped' && !now.self, 40);
  assert.equal(stopped.enabled, false);
  assert.equal(stopped.authKeySet, false);
  assert.deepEqual(await config(mini), { enabled: false, authKeySet: false, controlURL: DEFAULT_CONTROL_URL });
  assert.notEqual(await tunnelHttp(address, 3000), 200, 'the node does not answer over the tunnel');
});

test('log out erases the node identity and the key: an enable asks for a login, and the next join is a new node', { timeout: 600_000 }, async t => {
  if (!KEY_FILE) {
    t.skip(NO_KEY);
    return;
  }
  const mini = await connect();
  t.after(async () => {
    try {
      await cleanState(mini);
    } finally {
      await mini.close();
    }
  });
  await cleanState(mini);
  const first = await join(mini);
  assert.equal(first.loggedIn, true, 'a joined node reports its identity');
  const address = tailnetAddress(first);
  await mini.rpc('logoutTailscale');
  const out = await until(() => status(mini), now => now.backendState === 'Stopped' && !now.self && now.loggedIn === false, 40);
  assert.equal(out.enabled, false);
  assert.equal(out.authKeySet, false);
  assert.notEqual(await tunnelHttp(address, 3000), 200, 'the old node does not answer over the tunnel');

  // Without a key, enabling asks for a login: no identity is left to reconnect with.
  await mini.rpc('setTailscaleConfig', { enabled: true });
  const login = await until(() => status(mini), now => now.backendState === 'NeedsLogin' && Boolean(now.authURL), 90);
  assert.equal(login.loggedIn, false);

  await mini.rpc('setTailscaleConfig', { enabled: false });
  const second = await join(mini);
  assert.notDeepEqual(second.self.tailscaleIPs, first.self.tailscaleIPs, 'the new login is a new node with another address');
  assert.equal(await tunnelHttp(tailnetAddress(second), 30_000), 200);

  // A restart right after the log out, while the client still stops, does not
  // keep the identity: the next start erases it.
  await mini.rpc('logoutTailscale');
  const restartAt = Date.now();
  mini.rpc('reboot', { force: true }, 5000).catch(() => undefined);
  await until(() => mini.restarted(restartAt), Boolean, 120);
  await mini.open();
  assert.equal((await status(mini)).loggedIn, false, 'no identity after the restart');
  await mini.rpc('setTailscaleConfig', { enabled: true });
  const again = await until(() => status(mini), now => now.backendState === 'NeedsLogin' && Boolean(now.authURL), 90);
  assert.equal(again.running, false, 'the old node does not come back');
});
