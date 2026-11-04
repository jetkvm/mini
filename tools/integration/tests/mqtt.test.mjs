import assert from 'node:assert/strict';
import net from 'node:net';
import { test } from 'node:test';
import { closeDiskDialogs, connectHost } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { mqttBroker, updateServer } from '../lib/servers.mjs';

// MQTT / Home Assistant integration against lib/mqtt_broker.mjs.

const SETTING_KEYS = ['enabled', 'broker', 'port', 'username', 'password', 'base_topic', 'use_tls', 'tls_insecure',
  'enable_ha_discovery', 'enable_actions', 'debounce_ms'];
// Under <base topic>/<device ID>/: the Mini's states, retained.
const STATES = ['status', 'video/state', 'cloud/state', 'sessions/state', 'usb/state', 'jiggler/state', 'network/state',
  'system/state', 'virtual_media/state'];
const COMMANDS = ['jiggler/set', 'reboot/set', 'virtual_media/set'];
// Under homeassistant/<component>/jetkvm_<device ID>/<object>/config: the
// entities that the Mini announces with remote control on.
const ENTITIES = ['binary_sensor/online', 'binary_sensor/video_signal', 'sensor/video_resolution', 'sensor/video_fps',
  'binary_sensor/cloud_connected', 'sensor/active_sessions', 'binary_sensor/usb_state', 'sensor/ip_address', 'sensor/hostname',
  'sensor/memory_used', 'sensor/storage_used', 'sensor/storage_free', 'select/virtual_media', 'switch/jiggler', 'button/reboot'];
// Entities of other firmware under the same identity, which the Mini removes
// with an empty retained config: it has no updater, no ATX or DC extension,
// and it measures neither its CPU load nor its temperature.
const REMOVED_ENTITIES = ['update/firmware', 'binary_sensor/power_led', 'binary_sensor/hdd_led', 'button/atx_power_short',
  'button/atx_power_long', 'button/atx_reset', 'sensor/voltage', 'sensor/current', 'sensor/power', 'switch/dc_power',
  'binary_sensor/dc_power', 'select/dc_restore', 'sensor/dc_restore', 'sensor/cpu_load', 'sensor/temperature'];
const NO_MEDIA = '-- no media --';
const MEMORY_FAILURES = ['admissionDenials', 'videoRebuildBeginDenials', 'videoRebuildFailures', 'jsonAllocationFailures'];
// esp_reset_reason(): a restart that the firmware asked for.
const ESP_RST_SW = 3;
// The Mini publishes its states every 15 seconds (MQTT_STATUS_INTERVAL_MS).
const REFRESH_LIMIT_S = 20;

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const pick = settings => Object.fromEntries(SETTING_KEYS.map(key => [key, settings[key]]));

async function memory(mini) {
  const response = await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(5000) });
  const budget = (await response.json()).memoryBudget ?? {};
  return { failures: Object.fromEntries(MEMORY_FAILURES.map(key => [key, Number(budget[key] ?? 0)])), tls: Number(budget.activeTlsReservations) };
}

/**
 * Points the Mini at a new broker with discovery and actions on. Use
 * beforeClose(cleanup) for RPC cleanup: t.after() hooks run in registration
 * order, so a later one would find the browser closed.
 */
async function onBroker(t) {
  const mini = await connect();
  let broker = null;
  const cleanups = [];
  t.after(async () => {
    try {
      for (const cleanup of cleanups.reverse()) await cleanup();
    } finally {
      await mini.close().finally(() => broker?.close());
    }
  });
  broker = await mqttBroker(mini);
  const deviceId = await mini.rpc('getDeviceID');
  assert.ok(deviceId, 'getDeviceID');
  const found = await mini.rpc('getMqttSettings');
  for (const key of SETTING_KEYS) assert.ok(key in found, `getMqttSettings reports ${key}`);
  const before = await memory(mini);
  // A base topic of this run alone.
  const baseTopic = `integration-${Date.now().toString(16)}`;
  const settings = pick({ ...found, enabled: true, broker: broker.host, port: broker.port, base_topic: baseTopic, use_tls: false,
    tls_insecure: false, enable_ha_discovery: true, enable_actions: true });
  const clientId = `jetkvm-${deviceId}`;
  await mini.rpc('setMqttSettings', { settings }, 30_000);
  await until(() => broker.connects(clientId), connects => connects >= 1, 10);
  const status = await until(() => mini.rpc('getMqttStatus'), now => now.connected === true, 15);
  assert.equal(typeof status.connected, 'boolean', 'getMqttStatus reports connected');
  assert.deepEqual(pick(await mini.rpc('getMqttSettings')), settings, 'the Mini reads back the settings that it stored');
  const base = `${baseTopic}/${deviceId}`;
  return {
    mini, broker, clientId, base, settings,
    config: entity => `homeassistant/${entity.replace('/', `/jetkvm_${deviceId}/`)}/config`,
    sent: suffix => broker.messages(`${base}/${suffix}`).filter(message => message.origin === 'client' && message.clientId === clientId)
      .map(message => JSON.parse(message.payload)),
    beforeClose: cleanup => cleanups.push(cleanup),
    memoryUnchanged: async () => {
      const now = await memory(mini);
      assert.deepEqual(now.failures, before.failures, 'the Mini counted no refused or failed allocation');
      assert.equal(now.tls, 0, 'the Mini holds no TLS reservation for a connection without TLS');
    },
  };
}

/** Waits until the broker keeps every state and every entity of the Mini, and returns what it keeps. */
function announced({ broker, base, config }) {
  const required = [...STATES.map(suffix => `${base}/${suffix}`), ...ENTITIES.map(config)];
  return until(() => broker.retained(), retained => required.every(topic => topic in retained), 15);
}

test('after connecting, the Mini keeps its states and its Home Assistant entities on the MQTT broker', { timeout: 180_000 }, async t => {
  const on = await onBroker(t);
  const { mini, broker, clientId, base, config } = on;
  const retained = await announced(on);
  const host = new URL(mini.url).hostname;
  const payload = topic => JSON.parse(retained[topic].payload);

  for (const topic of [...STATES.map(suffix => `${base}/${suffix}`), ...ENTITIES.map(config)]) {
    const { qos, retain, clientId: from } = retained[topic];
    assert.deepEqual({ qos, retain, from }, { qos: 1, retain: true, from: clientId }, `${topic} is retained with QoS 1 by the Mini`);
  }

  // The entities: exactly those of the list, each on a state that the Mini
  // keeps and with a command that the Mini has subscribed to.
  const commands = await until(() => broker.subscriptions(clientId), now => Object.keys(now ?? {}).length === COMMANDS.length, 5);
  assert.deepEqual(commands, Object.fromEntries(COMMANDS.map(suffix => [`${base}/${suffix}`, 1])));
  const configs = Object.keys(retained).filter(topic => topic.startsWith('homeassistant/'));
  assert.deepEqual(configs.sort(), ENTITIES.map(config).sort(), 'the entities that the broker keeps');
  for (const entity of ENTITIES) {
    const entry = payload(config(entity));
    assert.equal(entry.availability_topic, `${base}/status`, `${entity} is available with the Mini's status`);
    if (entity !== 'button/reboot') assert.ok(entry.state_topic in retained, `${entity} reads a state that the Mini keeps: ${entry.state_topic}`);
    if (entry.command_topic) assert.ok(entry.command_topic in commands, `${entity} commands a topic of the Mini: ${entry.command_topic}`);
  }
  assert.equal(payload(config('switch/jiggler')).command_topic, `${base}/jiggler/set`);
  assert.equal(payload(config('select/virtual_media')).command_topic, `${base}/virtual_media/set`);
  assert.ok(payload(config('select/virtual_media')).options.includes(NO_MEDIA), 'virtual media offers to take the image out');
  assert.equal(payload(config('button/reboot')).command_topic, `${base}/reboot/set`);
  assert.match(payload(config('sensor/video_resolution')).value_template, /'No signal'/, 'the resolution without a signal');
  // An entity that the Mini does not have gets an empty retained config,
  // which removes it from Home Assistant.
  await until(() => REMOVED_ENTITIES.filter(entity => !broker.messages(config(entity)).some(message => message.retain && message.payload === '')),
    missing => missing.length === 0, 15);

  // The states, against what the Mini says over its RPC.
  assert.equal(payload(`${base}/status`).online, true);
  const video = payload(`${base}/video/state`);
  if (video.ready === true) assert.ok(video.width > 0 && video.height > 0 && video.fps >= 0, `video/state with a signal: ${JSON.stringify(video)}`);
  else assert.deepEqual(video, { ready: false, error: 'no_signal', width: 0, height: 0, fps: 0 });
  assert.equal(typeof payload(`${base}/cloud/state`).connected, 'boolean');
  assert.ok([0, 1].includes(payload(`${base}/sessions/state`).active_sessions));
  assert.ok(['not attached', 'attached', 'configured', 'suspended'].includes(payload(`${base}/usb/state`).state));
  assert.equal(payload(`${base}/jiggler/state`).enabled, await mini.rpc('getJigglerState'), 'the jiggler state is the Mini\'s');
  if (net.isIPv4(host)) assert.equal(payload(`${base}/network/state`).ip_address, host);
  assert.ok(payload(`${base}/network/state`).hostname);
  const system = payload(`${base}/system/state`);
  assert.ok(system.memory_used > 0 && system.memory_total >= system.memory_used, `system/state: ${JSON.stringify(system)}`);
  // The storage fields are there with a TF card and left out without one.
  const card = await mini.rpc('getStorageSpace').then(() => true, () => false);
  assert.deepEqual(['storage_used', 'storage_free'].filter(field => typeof system[field] === 'number'),
    card ? ['storage_used', 'storage_free'] : [], `system/state ${card ? 'with' : 'without'} a TF card: ${JSON.stringify(system)}`);
  const media = await mini.rpc('getVirtualMediaState');
  if (media === null) assert.deepEqual(payload(`${base}/virtual_media/state`), { mounted_image: NO_MEDIA, source: 'none' });
  else assert.notEqual(payload(`${base}/virtual_media/state`).source, 'none');

  // The Mini publishes its status again without a change.
  const published = on.sent('status').length;
  await until(() => on.sent('status').length, now => now > published, REFRESH_LIMIT_S);
  await on.memoryUnchanged();
});

test('an MQTT jiggler command switches the jiggler, and the Mini publishes the new state', { timeout: 120_000 }, async t => {
  const on = await onBroker(t);
  const { mini, broker, base } = on;
  const found = await mini.rpc('getJigglerState');
  assert.equal(typeof found, 'boolean', 'getJigglerState returns a boolean');
  assert.equal(broker.publish(`${base}/jiggler/set`, found ? 'OFF' : 'ON'), 1, 'the Mini got the command');
  await until(() => mini.rpc('getJigglerState'), enabled => enabled === !found, 5);
  await until(() => on.sent('jiggler/state').at(-1), state => state?.enabled === !found, 5);
  assert.equal(broker.publish(`${base}/jiggler/set`, found ? 'ON' : 'OFF'), 1, 'the Mini got the command');
  await until(() => mini.rpc('getJigglerState'), enabled => enabled === found, 5);
  await on.memoryUnchanged();
});

// The Mini publishes its states from its own task while its MQTT client
// takes the commands. Fifty commands a second that the Mini rejects stop
// neither over two rounds of states.
test('MQTT commands that the Mini rejects do not hold up its states', { timeout: 120_000 }, async t => {
  const on = await onBroker(t);
  const { mini, broker, base } = on;
  const found = await mini.rpc('getJigglerState');
  const published = on.sent('status').length;
  let sending = true;
  let sent = 0;
  let delivered = 0;
  const commands = (async () => {
    while (sending) {
      sent += 1;
      delivered += broker.publish(`${base}/jiggler/set`, 'integration-invalid');
      await sleep(20);
    }
  })();
  try {
    await until(() => on.sent('status').length, now => now >= published + 1, REFRESH_LIMIT_S);
    await until(() => on.sent('status').length, now => now >= published + 2, REFRESH_LIMIT_S);
  } finally {
    sending = false;
    await commands;
  }
  assert.equal(delivered, sent, 'the Mini\'s connection took every command');
  assert.equal(await mini.rpc('getJigglerState'), found, 'a rejected command leaves the jiggler');
  await on.memoryUnchanged();
});

test('the Mini connects and subscribes again after the MQTT broker closed its connection', { timeout: 120_000 }, async t => {
  const on = await onBroker(t);
  const { mini, broker, clientId, base } = on;
  const connects = broker.connects(clientId);
  assert.equal(broker.drop(clientId), true, 'the broker closed the Mini\'s connection');
  await until(() => broker.connects(clientId), now => now > connects, 25);
  await until(() => mini.rpc('getMqttStatus'), status => status.connected === true, 10);
  const want = Object.fromEntries(COMMANDS.map(suffix => [`${base}/${suffix}`, 1]));
  await until(() => broker.subscriptions(clientId), now => assert.deepEqual(now, want) ?? true, 5);
  await on.memoryUnchanged();
});

test('the Mini comes back to an MQTT broker that was away, and publishes its states again', { timeout: 180_000 }, async t => {
  const on = await onBroker(t);
  const { mini, broker, clientId, base } = on;
  await announced(on);
  const connects = broker.connects(clientId);
  await broker.stop();
  await until(() => mini.rpc('getMqttStatus'), status => status.connected === false, 10);
  // The Mini tries again every 10 seconds (MQTT_RECONNECT_INTERVAL_MS). The
  // broker comes back after an attempt that found it away.
  await until(() => broker.turnedAway(), attempts => attempts >= 1, 25);
  const backAt = Date.now();
  await broker.start();
  await until(() => broker.connects(clientId), now => now > connects, 25);
  await until(() => mini.rpc('getMqttStatus'), status => status.connected === true, 10);
  const again = topic => broker.messages(topic).some(message => message.at >= backAt && message.clientId === clientId && message.payload !== '');
  await until(() => [...STATES.map(suffix => `${base}/${suffix}`), ...ENTITIES.map(on.config)].filter(topic => !again(topic)),
    missing => missing.length === 0, 15);
  assert.equal(on.sent('status').at(-1).online, true);
  await on.memoryUnchanged();
});

// The media entity publishes a TF card file name or "-- no media --". A Linux
// host confirms the medium.
test('MQTT virtual media commands mount an image of the TF card and take it out', { timeout: 300_000 }, async t => {
  const on = await onBroker(t);
  const { mini, broker, base } = on;
  const filename = `integration-mqtt-${Date.now()}.img`;
  let server = null;
  let watch = null;
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'the Mini has no image mounted before the test');
  on.beforeClose(async () => {
    server?.close();
    await watch?.stop();
    try {
      // Only the image of this test is taken out.
      if ((await mini.rpc('getVirtualMediaState', {}, 30_000))?.filename === filename) await mini.rpc('unmountImage', {}, 30_000);
    } finally {
      try {
        await closeDiskDialogs(mini);
      } finally {
        // By name: the Mini's list of files ends after 64 entries.
        for (const name of [filename, `${filename}.incomplete`]) {
          await mini.rpc('deleteStorageFile', { filename: name }).catch(error => {
            if (!/file does not exist/.test(error.message)) throw error;
          });
        }
      }
    }
  });
  const host = await connectHost(mini);
  let hostLine = null;
  if (host.system === 'linux') watch = host.stream(host.commands.watchMedia, line => { hostLine = line; });
  const hostMedium = () => (hostLine === null || hostLine === 'none' ? hostLine : host.commands.medium(hostLine.split(' ')).type);
  server = await updateServer(mini);
  await mini.rpc('downloadToStorage', { url: server.url, filename });
  await until(() => mini.rpc('getStorageDownloadState'), download => download.state === 'done' && download.filename === filename, 60);

  const startedAt = Date.now();
  assert.equal(broker.publish(`${base}/virtual_media/set`, filename), 1, 'the Mini got the command');
  // The Mini answers no call about its media while it gives the computer the
  // drive of the image; the page's mount call has 60 seconds.
  await until(() => mini.rpc('getVirtualMediaState'), media => media?.filename === filename && media.mode === 'Disk', 60);
  await until(() => on.sent('virtual_media/state').at(-1), state => state.mounted_image === filename && state.source === 'storage', 5);
  if (watch) await until(hostMedium, medium => medium === 'disk', 30);
  // The image stays in the drive: the Mini names it at every second of a
  // minute and in every state that it publishes in that minute.
  const published = on.sent('virtual_media/state').length;
  for (const end = Date.now() + 60_000; Date.now() < end; await sleep(1000)) {
    assert.equal((await mini.rpc('getVirtualMediaState'))?.filename, filename, 'the image is still mounted');
  }
  const states = on.sent('virtual_media/state').slice(published);
  assert.ok(states.length >= 3, `the Mini published its states in that minute: ${states.length}`);
  for (const state of states) assert.deepEqual(state, { mounted_image: filename, source: 'storage' });
  if (watch) assert.equal(hostMedium(), 'disk', 'the host still has the disk');

  assert.equal(broker.publish(`${base}/virtual_media/set`, NO_MEDIA), 1, 'the Mini got the command');
  // The walk gives its unmount call 30 seconds.
  await until(() => mini.rpc('getVirtualMediaState'), media => media === null, 30);
  await until(() => on.sent('virtual_media/state').at(-1), state => state.mounted_image === NO_MEDIA && state.source === 'none', 5);
  if (watch) await until(hostMedium, medium => medium === 'none', 30);
  // A Mini that restarts comes back to the broker with nothing mounted, so
  // the checks above do not tell a restart after the unmount.
  assert.equal(await mini.restarted(startedAt), false, 'the Mini has not restarted since the first command');
  await on.memoryUnchanged();
});

test('the MQTT reboot command restarts the Mini, and the Mini comes back to the broker', { timeout: 300_000 }, async t => {
  const on = await onBroker(t);
  const { mini, broker, clientId, base } = on;
  const connects = broker.connects(clientId);
  const startedAt = Date.now();
  assert.equal(broker.publish(`${base}/reboot/set`, 'PRESS'), 1, 'the Mini got the command');
  await until(() => mini.restarted(startedAt), restarted => restarted, 60);
  assert.equal((await mini.status()).resetReason, ESP_RST_SW, 'the restart was the firmware\'s');
  // Before its restart the Mini is offline for the broker: by its own
  // message, or by its last will when the connection ended first.
  const statuses = () => broker.messages(`${base}/status`).filter(message => message.at >= startedAt && message.clientId === clientId)
    .map(message => JSON.parse(message.payload).online);
  assert.ok(statuses().includes(false), `a status after the command says offline: ${JSON.stringify(statuses())}`);
  // The stored settings bring the Mini back to the broker after its start,
  // and its status after the offline one says online.
  await until(() => broker.connects(clientId), now => now > connects, 60);
  await until(statuses, now => now.at(-1) === true && now.lastIndexOf(false) >= 0, 15);
  await mini.open();
  await until(() => mini.rpc('getMqttStatus'), status => status.connected === true, 15);
});

test('a Mini whose MQTT is switched off removes what it kept on the broker', { timeout: 120_000 }, async t => {
  const on = await onBroker(t);
  const { mini, broker, base, settings } = on;
  await announced(on);
  const off = { ...settings, enabled: false };
  await mini.rpc('setMqttSettings', { settings: off }, 30_000);
  await until(async () => pick(await mini.rpc('getMqttSettings')), now => assert.deepEqual(now, off) ?? true, 10);
  await until(() => mini.rpc('getMqttStatus'), status => status.connected === false, 10);
  // Only a status that says offline may stay.
  const left = () => Object.keys(broker.retained()).filter(topic => topic.startsWith('homeassistant/') || (topic.startsWith(`${base}/`) && topic !== `${base}/status`));
  await until(left, topics => topics.length === 0, 10);
  const status = broker.retained()[`${base}/status`];
  if (status) assert.equal(JSON.parse(status.payload).online, false, 'the status that the broker keeps');
  await on.memoryUnchanged();
});

// "Test Connection" of the settings page: the Mini connects once with
// settings that it does not store, and says why a broker refused it.
const CREDENTIALS = { username: 'mqtt-integration', password: 'mqtt-integration-password' };
const PROBES = {
  'an MQTT connection test tells the right user name and password from wrong ones': {
    secure: false,
    cases: [
      { name: 'the right credentials connect', success: true, changes: {} },
      { name: 'a wrong password is refused', success: false, changes: { password: 'wrong' } },
      { name: 'a wrong user name is refused', success: false, changes: { username: 'wrong' } },
      { name: 'no credentials are refused', success: false, changes: { username: '', password: '' } },
      { name: 'the right credentials connect again', success: true, changes: {} },
    ],
  },
  'an MQTT connection test refuses a broker certificate that the Mini does not trust': {
    secure: true,
    cases: [
      { name: 'TLS without verification connects', success: true, changes: { tls_insecure: true } },
      { name: 'the certificate is refused', success: false, changes: { tls_insecure: false } },
      { name: 'TLS without verification connects again', success: true, changes: { tls_insecure: true } },
    ],
  },
};
for (const [title, { secure, cases }] of Object.entries(PROBES)) {
  test(title, { timeout: 120_000 }, async t => {
    const mini = await connect();
    let broker = null;
    t.after(() => mini.close().finally(() => broker?.close()));
    broker = await mqttBroker(mini, { credentials: CREDENTIALS, secure });
    const found = await mini.rpc('getMqttSettings');
    const accepted = entries => entries.some(entry => entry.type === 'connect' && entry.accepted);
    try {
      for (const { name, success, changes } of cases) {
        const from = broker.log.length;
        const result = await mini.rpc('testMqttConnection', { settings: { ...found, ...CREDENTIALS, enabled: true, broker: broker.host,
          port: broker.port, use_tls: secure, tls_insecure: false, ...changes } }, 15_000);
        assert.equal(result.success, success, `${name}: ${JSON.stringify(result)}`);
        if (success) {
          await until(() => broker.log.slice(from), accepted, 5);
          continue;
        }
        assert.ok(result.error, `${name}: the Mini gives a reason`);
        assert.doesNotMatch(result.error, /timed? out|timeout/i, name);
        if (!secure) assert.match(result.error, /bad user name or password/i, name);
        const refusal = entry => (secure
          ? entry.type === 'tlsRejected' && /alert (unknown ca|bad certificate|certificate unknown)/i.test(entry.error)
          : entry.type === 'connect' && !entry.accepted);
        await until(() => broker.log.slice(from), entries => entries.some(refusal), 5);
        assert.equal(accepted(broker.log.slice(from)), false, `${name}: the broker accepted no connection`);
      }
    } finally {
      assert.deepEqual(await mini.rpc('getMqttSettings'), found, 'a connection test stores nothing');
    }
  });
}
