// Record and restore what a settings erase (factory reset, update with
// resetConfig) loses.
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { isDeepStrictEqual, promisify } from 'node:util';
import { benchFile, readBench } from '../../combined_load/runner/build.mjs';
import { quote } from '../../combined_load/runner/hosts.mjs';
import { SSH_OPTIONS } from '../../combined_load/runner/switches.mjs';
import { sender } from './approval.mjs';
import { SETTINGS, until } from './mini.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const run = promisify(execFile);

// Restorable settings beyond SETTINGS: [getter, setter, value → params,
// compared part of the value (default: all)].
const whole = value => value;
const STORED = [
  ['getKeyboardLayout', 'setKeyboardLayout', layout => ({ layout })],
  ['getBacklightSettings', 'setBacklightSettings', params => ({ params })],
  ['getDisplayRotation', 'setDisplayRotation', params => ({ params })],
  ['getVideoCodecPreference', 'setVideoCodecPreference', codec => ({ codec })],
  ['getEDID', 'setEDID', edid => ({ edid })],
  ['getVideoSleepMode', 'setVideoSleepMode', ({ duration }) => ({ duration })],
  ['getHostDisplayIdleMode', 'setHostDisplayIdleMode', mode => mode],
  ['getAudioConfig', 'setAudioConfig', params => ({ params })],
  ['getUsbDevices', 'setUsbDevices', devices => ({ devices })],
  ['getUsbConfig', 'setUsbConfig', usbConfig => ({ usbConfig })],
  ['getUsbEmulationState', 'setUsbEmulationState', enabled => ({ enabled })],
  ['getCloudState', 'setCloudUrl', ({ url, appUrl }) => ({ apiUrl: url, appUrl }), ({ url, appUrl }) => ({ url, appUrl })],
  // An erase turns automatic updates back on.
  ['getAutoUpdateState', 'setAutoUpdateState', enabled => ({ enabled })],
].map(([read, store, parameters, part = whole]) => ({ read, store, parameters, part }));

// Every readable stored value (SETTINGS, STORED, Wi-Fi, device ID, OS
// Services key, isSetup), for comparing across an operation that must keep them.
export async function storedSettings(mini) {
  const { rpc } = mini;
  const values = {};
  for (const [name, setting] of Object.entries(SETTINGS)) values[name] = await setting.read(rpc);
  for (const { read, part } of STORED) values[read] = part(await rpc(read));
  const { band, networks } = await rpc('getWifiSettings');
  values.wifi = { band, networks: networks.map(network => network.ssid) };
  values.deviceId = await rpc('getDeviceID');
  values.osServicesKey = (await rpc('getVideoState')).osServices.fingerprint;
  values.isSetup = (await (await fetch(new URL('/device/status', mini.url), { signal: AbortSignal.timeout(10_000) })).json()).isSetup;
  return values;
}

// The bench file plus `wifi` and `skip` (reason an erase test cannot run on
// this bench, or null).
export function eraseBench() {
  const bench = readBench(benchFile(root, process.env.INTEGRATION_BENCH));
  const wifi = bench.JETKVM_SKU === 'jetkvm-mini-wireless';
  const skip = wifi && !(bench.JETKVM_WIFI_SSID && bench.JETKVM_REMOTE_HOST)
    ? 'the erase removes the Mini\'s Wi-Fi network: the bench file needs JETKVM_WIFI_SSID and a host that sends it over BLE'
    : null;
  return { ...bench, wifi, skip };
}

/**
 * Starts tools/e2e/wifi_reprovision.py detached on the host, so a killed test
 * still gets the erased Mini back on Wi-Fi over BLE setup. Returns
 * done(wasErased), which waits for it (after an erase) and cleans up.
 */
async function wifiSetup(bench, mini, version) {
  const remote = bench.JETKVM_REMOTE_HOST;
  // POSIX sh; the host's login shell may differ.
  const onHost = async command => (await run('ssh', [...SSH_OPTIONS, remote, `sh -c ${quote(command)}`], { timeout: 30_000 })).stdout.trim();
  assert.match(version, /\+codelog$/, 'a bench build with the fixed BLE setup code runs after the erase');
  const mac = (await mini.rpc('getNetworkState')).mac_address;
  const name = `JetKVM_${mac.split(':').slice(3).join('').toUpperCase()}`;
  const directory = await onHost('mktemp -d /tmp/jetkvm-reprovision.XXXXXX');
  const scripts = ['wifi_reprovision.py', 'ble_setup_client.py'].map(file => path.join(root, 'tools', 'e2e', file));
  await run('scp', ['-q', ...SSH_OPTIONS, ...scripts, `${remote}:${directory}/`], { timeout: 30_000 });
  // The Wi-Fi password comes from the host's env file.
  await onHost(`cd ${directory} && set -a && . ~/.config/jetkvm-e2e.env && set +a
nohup python3 wifi_reprovision.py --host ${new URL(mini.url).hostname} --name ${name} \
--ssid ${quote(bench.JETKVM_WIFI_SSID)} --adapter ${bench.JETKVM_BLE_ADAPTER ?? 'hci0'} </dev/null >log 2>&1 &
echo $! >pid
sleep 1
kill -0 $(cat pid) || { cat log >&2; exit 1; }`);
  return async wasErased => {
    const ended = wasErased && await until(() => onHost(`kill -0 $(cat ${directory}/pid) 2>/dev/null && echo runs || echo ended`),
      state => state === 'ended', 180).then(() => true, () => false);
    const log = await onHost(`${ended ? '' : `kill $(cat ${directory}/pid); `}cat ${directory}/log; rm -r ${directory}`);
    if (wasErased && !ended) throw new Error(`the Mini was not put back on its network:\n${log}`);
  };
}

/**
 * Records STORED and the OS Services key; refuses states that cannot be
 * restored (a mount, Wi-Fi other than the bench's network on auto). Returns
 * { wifiSetup(version), restore(), wifiDone(wasErased) }; restore() also
 * moves the sender's approval to the Mini's new key. Call wifiSetup before the erase.
 */
export async function beforeErase(mini, bench) {
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'no medium is mounted');
  const { band, networks } = await mini.rpc('getWifiSettings');
  assert.deepEqual({ band, networks: networks.map(network => network.ssid) },
    { band: 'auto', networks: bench.wifi ? [bench.JETKVM_WIFI_SSID] : [] }, 'the Mini has no Wi-Fi network but the bench\'s, on any band');
  const found = [];
  for (const setting of STORED) found.push({ ...setting, value: await mini.rpc(setting.read) });
  const key = (await mini.rpc('getVideoState')).osServices;
  let done = null;
  const restore = async () => {
    const failures = [];
    for (const { read, store, parameters, part, value } of found) {
      if (isDeepStrictEqual(part(await mini.rpc(read)), part(value))) continue;
      await mini.rpc(store, parameters(value), 30_000).catch(error => failures.push(error.message));
    }
    for (const { read, part, value } of found) {
      const now = await mini.rpc(read);
      if (!isDeepStrictEqual(part(now), part(value))) failures.push(`${read} is ${JSON.stringify(now)} and was ${JSON.stringify(value)}`);
    }
    const now = (await mini.rpc('getVideoState')).osServices;
    if (mini.remoteHost && key.fingerprint && now.fingerprint !== key.fingerprint) {
      const approved = await sender(mini, '--forget-kvm', key.fingerprint).then(() => true, error => {
        if (/is not approved|no installed OS Services sender/.test(error.message)) return false;
        throw error;
      });
      if (approved) await sender(mini, '--approve-kvm', now.fingerprint);
      if (approved && key.connected && !key.approvalPending) {
        await until(() => mini.rpc('getVideoState'), state => state.osServices.connected && !state.osServices.approvalPending, 60);
      }
    }
    assert.deepEqual(failures, [], 'the Mini\'s settings are as before the erase');
  };
  return {
    wifiSetup: async version => {
      if (bench.wifi) done = await wifiSetup(bench, mini, version ?? (await mini.rpc('getLocalVersion')).systemVersion);
    },
    restore,
    wifiDone: async wasErased => done?.(wasErased),
  };
}
