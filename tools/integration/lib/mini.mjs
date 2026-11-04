// The bench's Mini: JSON-RPC over a hidden browser page's data channel, plus
// its /diagnostics. run.sh sets INTEGRATION_BENCH and INTEGRATION_IMAGE.
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { isDeepStrictEqual } from 'node:util';
import { benchFile, readBench } from '../../combined_load/runner/build.mjs';
import { launchBrowser, pageRpc, readDiagnostics } from '../../combined_load/runner/page.mjs';
import { STORED_SETTINGS, setDefaults } from '../../combined_load/runner/settings.mjs';
import { readFirmware } from '../../combined_load/runner/switches.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');

// Stored settings a test may change: connect() records them, close() restores
// them in this order.
export const SETTINGS = {
  updateApiUrl: { read: rpc => rpc('getUpdateApiUrl'), store: (rpc, url) => rpc('setUpdateApiUrl', { url }) },
  devChannel: { read: rpc => rpc('getDevChannelState'), store: (rpc, enabled) => rpc('setDevChannelState', { enabled }) },
  keyboardMacros: { read: rpc => rpc('getKeyboardMacros'), store: (rpc, macros) => rpc('setKeyboardMacros', { params: { macros } }) },
  jigglerConfig: { read: rpc => rpc('getJigglerConfig'), store: (rpc, jigglerConfig) => rpc('setJigglerConfig', { jigglerConfig }) },
  jiggler: { read: rpc => rpc('getJigglerState'), store: (rpc, enabled) => rpc('setJigglerState', { enabled }) },
  wakeOnLanDevices: { read: rpc => rpc('getWakeOnLanDevices'), store: (rpc, devices) => rpc('setWakeOnLanDevices', { params: { devices } }) },
  networkSettings: { read: rpc => rpc('getNetworkSettings'), store: (rpc, settings) => rpc('setNetworkSettings', { settings }) },
  // The Mini applies a stream quality shortly after its answer.
  streamQuality: {
    read: rpc => rpc('getStreamQualityFactor'),
    store: async (rpc, factor) => {
      await rpc('setStreamQualityFactor', { factor });
      await until(() => rpc('getStreamQualityFactor'), now => now === factor, 5);
    },
  },
  // An adaptation that is not set reads as no field, and is stored as "".
  osServices: {
    read: rpc => rpc('getOsServicesSettings'),
    store: (rpc, settings) => rpc('setOsServicesSettings', { settings: { ...settings, adaptation: settings.adaptation ?? '' } }),
  },
  // The password reads back masked; storing the mask keeps it. Leaving a
  // broker can take a few seconds.
  mqtt: { read: rpc => rpc('getMqttSettings'), store: (rpc, settings) => rpc('setMqttSettings', { settings }, 30_000) },
  videoCodec: { read: rpc => rpc('getVideoCodecPreference'), store: (rpc, codec) => rpc('setVideoCodecPreference', { codec }) },
  hostDisplayIdle: { read: rpc => rpc('getHostDisplayIdleMode'), store: (rpc, { enabled }) => rpc('setHostDisplayIdleMode', { enabled }) },
};

// Only a mock-cloud build trusts the stand-in HTTPS servers' test CA and reads
// OS Services releases from the update server's address.
export const mockCloudBuild = version => version.includes('mockcloud');

// Polls read() every 250 ms until check(value) is true and returns the value.
// A throwing read() counts as no value. Throws after `seconds`.
export async function until(read, check, seconds) {
  const end = Date.now() + seconds * 1000;
  for (;;) {
    let value;
    let error;
    try {
      value = await read();
      if (await check(value)) return value;
    } catch (e) {
      error = e;
    }
    if (Date.now() >= end) {
      throw new Error(`not reached after ${seconds} s; last: ${error ? error.message : JSON.stringify(value)}`);
    }
    await new Promise(resolve => setTimeout(resolve, 250));
  }
}

// Runs in the page: records data channels by label (used by lib/keys.mjs).
function keepChannels() {
  window.__integrationChannels = {};
  const create = RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel = function (label, ...rest) {
    return (window.__integrationChannels[label] = create.call(this, label, ...rest));
  };
}

const videoOf = page => () => page.evaluate(async () => (await window.__kvmTestHooks?.getInboundVideoStats?.()) ?? null);

/**
 * Opens a session on the bench's Mini, turns automatic updates off and records
 * SETTINGS. See the returned object for the API; `close()` restores SETTINGS.
 * `session()` opens a second page that takes over the Mini's single session.
 */
export async function connect() {
  const { INTEGRATION_BENCH: bench, INTEGRATION_IMAGE: imageFile } = process.env;
  if (!bench || !imageFile) throw new Error('start the tests with tools/integration/run.sh <bench>');
  const settings = readBench(benchFile(root, bench));
  const url = settings.JETKVM_URL;
  const remoteHost = settings.JETKVM_COMBINED_LOAD_HOST || settings.JETKVM_REMOTE_HOST;
  const image = readFirmware(imageFile);
  const browser = await launchBrowser(root);
  try {
    const page = await browser.newPage();
    await page.addInitScript(keepChannels);
    const rpc = pageRpc(page);
    const origin = new URL(url).origin;
    // Through the page's browser so the login cookie applies; maxRetries
    // covers keep-alive connections reset by a Mini restart.
    const status = async () => {
      const response = await page.request.get(`${origin}/diagnostics`, { maxRetries: 2, timeout: 10_000 });
      if (!response.ok()) throw new Error(`/diagnostics: ${response.status()}`);
      return readDiagnostics(await response.body());
    };
    const http = async (pathname, { method = 'GET', body } = {}) => (await page.request.fetch(origin + pathname,
      { method, data: body, headers: { Origin: origin }, maxRetries: 2, timeout: 10_000 })).status();
    const restarted = async sinceMs => (await status()).uptimeSeconds < (Date.now() - sinceMs) / 1000 - 2;
    const listen = () => page.evaluate(() => {
      window.__integrationMessages = [];
      window.__kvmTestHooks._getRpcDataChannel().addEventListener('message', event => {
        try {
          window.__integrationMessages.push({ message: JSON.parse(event.data), at: Date.now() });
        } catch {
          // not JSON-RPC
        }
      });
    });
    const received = () => page.evaluate(() => (window.__integrationMessages ?? []).map(({ message }) => message));
    const timedEvents = method => page.evaluate(name => (window.__integrationMessages ?? [])
      .filter(({ message }) => message.method === name && message.id === undefined)
      .map(({ message, at }) => ({ at, params: message.params ?? {} })), method);
    const events = method => page.evaluate(name => (window.__integrationMessages ?? [])
      .filter(({ message }) => message.method === name && message.id === undefined).map(({ message }) => message.params), method);
    const send = (method, params = {}) => page.evaluate(([name, values]) => {
      const id = `integration-${Date.now()}-${Math.random().toString(36).slice(2)}`;
      window.__kvmTestHooks._getRpcDataChannel().send(JSON.stringify({ jsonrpc: '2.0', method: name, params: values, id }));
      return id;
    }, [method, params]);
    const video = videoOf(page);
    const frames = async () => (await video())?.framesDecoded ?? null;
    // A new image is confirmed 10 s after boot (OTA_CONFIRM_DELAY_MS) and the
    // Mini refuses updates until then.
    const open = async () => {
      await until(status, now => now.uptimeSeconds >= 15, 60);
      await until(() => page.goto(url, { waitUntil: 'domcontentloaded', timeout: 10_000 }), () => true, 60);
      await until(() => rpc('getUpdateApiUrl'), () => true, 60);
      await listen();
    };
    const leave = async () => {
      await page.goto('about:blank');
      await until(async () => (await fetch(new URL('/webrtc/stats', url), { signal: AbortSignal.timeout(5000) })).json(),
        stats => stats.sessionActive === false && stats.peerConnected === false, 15);
    };
    await open();
    // Left off on purpose: a test build is older than the latest release and
    // would auto-update away from the image under test.
    await setDefaults(rpc, { settings: { automaticUpdates: STORED_SETTINGS.automaticUpdates } });
    const found = {};
    for (const [name, setting] of Object.entries(SETTINGS)) found[name] = await setting.read(rpc);
    const close = async () => {
      try {
        await open();
        for (const [name, setting] of Object.entries(SETTINGS)) {
          if (isDeepStrictEqual(await setting.read(rpc), found[name])) continue;
          // A boolean setter answers false (no error) when storing failed, but
          // the in-memory value still reads back.
          const stored = await setting.store(rpc, found[name]);
          const now = await setting.read(rpc);
          if (!isDeepStrictEqual(now, found[name]) || (typeof stored === 'boolean' && stored !== found[name])) {
            throw new Error(`the Mini did not store ${name} as ${JSON.stringify(found[name])} again`);
          }
        }
      } finally {
        await browser.close();
      }
    };
    const session = async () => {
      const other = await browser.newPage();
      await other.addInitScript(keepChannels);
      const call = pageRpc(other);
      await until(() => other.goto(url, { waitUntil: 'domcontentloaded', timeout: 10_000 }), () => true, 60);
      await until(() => call('getUpdateApiUrl'), () => true, 60);
      return { page: other, rpc: call, video: videoOf(other), close: () => other.context().close() };
    };
    return { url, remoteHost, image, page, rpc, send, received, status, restarted, events, timedEvents, frames, video, http, open, leave, session, close };
  } catch (error) {
    await browser.close();
    throw error;
  }
}
