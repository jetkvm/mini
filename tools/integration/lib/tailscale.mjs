// Tailscale helpers: settings, status, joining a real tailnet, HTTP through it.
import fs from 'node:fs';
import http from 'node:http';
import { tailnetPath } from '../../combined_load/runner/tailscale.mjs';
import { until } from './mini.mjs';

// The machine running the tests must be on the key's tailnet. Never print the key.
export const KEY_FILE = process.env.JETKVM_TAILSCALE_AUTH_KEY_FILE;
export const NO_KEY = 'JETKVM_TAILSCALE_AUTH_KEY_FILE is not set: no auth key for a real tailnet';
export const DEFAULT_CONTROL_URL = 'https://controlplane.tailscale.com';

export const authKey = () => fs.readFileSync(KEY_FILE, 'utf8').trim();
export const status = mini => mini.rpc('getTailscaleStatus');
export const config = mini => mini.rpc('getTailscaleConfig');
export const tailnetAddress = now => now.self?.tailscaleIPs?.find(ip => !ip.includes(':'));

// Resolves with the HTTP status or the error message.
export const tunnelHttp = (address, timeoutMs) => new Promise(resolve => {
  const request = http.get({ host: address, path: '/device/status', agent: false, timeout: timeoutMs }, response => {
    response.resume();
    response.on('end', () => resolve(response.statusCode));
  });
  request.on('timeout', () => request.destroy(new Error(`no answer in ${timeoutMs} ms`)));
  request.on('error', error => resolve(error.message));
});

export async function cleanState(mini) {
  await mini.rpc('setTailscaleConfig', { enabled: false, authKey: '' });
  await mini.rpc('setTailscaleControlURL', { controlURL: DEFAULT_CONTROL_URL });
  await until(() => status(mini), now => now.backendState === 'Stopped' && !now.self, 40);
}

// Resolves once the Mini is online and this machine sees it as a peer.
export async function join(mini) {
  await mini.rpc('setTailscaleConfig', { enabled: true, authKey: authKey() });
  const joined = await until(() => status(mini), now => now.running && now.self?.online && tailnetAddress(now), 120);
  await until(() => tailnetPath(tailnetAddress(joined)), path => path !== null, 60);
  return joined;
}
