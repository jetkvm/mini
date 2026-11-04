// Stand-in servers on this machine's address towards the Mini.
import https from 'node:https';
import { serverCertificate } from '../../combined_load/runner/fake_cloud.mjs';
import { RELEASES_URL, addressTowards, serve } from '../../combined_load/runner/switches.mjs';
import { startMqttBroker } from './mqtt_broker.mjs';

/**
 * Update server offering mini.image as 99.0.0, signed with the test release
 * key. offer: { version, signature: 'valid' | 'none' | 'empty' | 'other key' |
 * 'other image' }. Returns { base, served, close }.
 */
export async function updateServer(mini, offer = {}) {
  return serve(await addressTowards(new URL(mini.url).hostname), mini.image, offer);
}

/**
 * OS Services release bucket at <base>/os-services/, proxying (and caching)
 * the real bucket so files keep their real signatures. Returns { base,
 * file(path), put(path, bytes), requests, close }.
 */
export async function releaseBucket(mini) {
  const address = await addressTowards(new URL(mini.url).hostname);
  const files = new Map();
  const real = async path => {
    const response = await fetch(`${RELEASES_URL}/${path}`, { signal: AbortSignal.timeout(300_000) });
    if (response.status === 404) return null;
    if (!response.ok) throw new Error(`${RELEASES_URL}/${path}: ${response.status}`);
    return Buffer.from(await response.arrayBuffer());
  };
  const file = path => {
    if (!files.has(path)) {
      files.set(path, real(path).catch(error => {
        files.delete(path);
        throw error;
      }));
    }
    return files.get(path);
  };
  const requests = [];
  const server = https.createServer(serverCertificate(address), async (request, response) => {
    const { pathname } = new URL(request.url, 'https://localhost');
    const answer = (status, body = Buffer.alloc(0)) => {
      requests.push({ path: pathname, status, at: Date.now() });
      // The Mini requires Content-Length.
      response.writeHead(status, { 'Content-Length': body.length }).end(body);
    };
    if (!pathname.startsWith('/os-services/')) return answer(404);
    try {
      const body = await file(pathname.slice('/os-services/'.length));
      answer(body ? 200 : 404, body ?? undefined);
    } catch {
      answer(502);
    }
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, resolve);
  });
  return {
    base: `https://${address}:${server.address().port}`,
    file,
    put: (path, bytes) => void files.set(path, Promise.resolve(bytes)),
    requests,
    close: () => {
      server.closeAllConnections();
      server.close();
    },
  };
}

// options: { credentials, secure } (secure uses a cert the Mini does not trust).
export async function mqttBroker(mini, options = {}) {
  return startMqttBroker({ host: await addressTowards(new URL(mini.url).hostname), ...options });
}
