// tailscale: the Mini on a tailnet, and the tool's browser session with it
// over the tailnet. The Mini joins with the auth key in the file that
// JETKVM_TAILSCALE_AUTH_KEY_FILE names, as a user enters a key in the
// settings page, and the page then loads the device page from the Mini's
// tailnet address. The machine that runs the tool must be on that tailnet.
import { execFile } from 'node:child_process';
import fs from 'node:fs';
import { promisify } from 'node:util';

const run = promisify(execFile);

/**
 * How this machine's Tailscale reaches a tailnet address: 'direct' when it
 * has a direct UDP path to that node, 'derp' when its packets go through a
 * DERP relay, or null when this machine has no Tailscale or no such peer.
 * The answer comes from `tailscale status --json`, which reads and changes
 * nothing.
 */
export async function tailnetPath(address) {
  try {
    const { stdout } = await run('tailscale', ['status', '--json'], { timeout: 5000 });
    const peer = Object.values(JSON.parse(stdout).Peer ?? {}).find(node => node.TailscaleIPs?.includes(address));
    if (!peer) return null;
    return peer.CurAddr ? 'direct' : 'derp';
  } catch {
    return null;
  }
}

/**
 * The switch of tailscale. page: the tool's page. url: the Mini's LAN
 * address. rpc(method, params, timeoutMs): the Mini's JSON-RPC through the
 * page. keyFile: the file with the auth key.
 *
 * On: the Mini gets Tailscale enabled with the key (setTailscaleConfig, as the
 * settings page's "Connect with Auth Key"), and when it reports its tailnet
 * address the page loads the device page from that address: the page's
 * session, its video, keys and calls go through the tunnel. Off: the page
 * loads the device page from the LAN address again, and the Mini gets
 * Tailscale disabled and its key removed, so that it keeps no key after a run.
 */
export function tailscaleSwitch({ page, url, rpc, keyFile, path = tailnetPath }) {
  let address = null;
  let pathNow = null;
  let pathAt = 0;
  const onTailnet = () => {
    try {
      return address !== null && new URL(page.url()).hostname === address;
    } catch {
      return false;
    }
  };
  return {
    prepare: async () => {
      if (!fs.existsSync(keyFile)) throw new Error(`JETKVM_TAILSCALE_AUTH_KEY_FILE names ${keyFile}, which does not exist`);
      return {};
    },
    on: async () => {
      const authKey = fs.readFileSync(keyFile, 'utf8').trim();
      await rpc('setTailscaleConfig', { enabled: true, authKey });
      const end = Date.now() + 60_000;
      let status;
      for (;;) {
        status = await rpc('getTailscaleStatus');
        if (status.running && status.self?.tailscaleIPs?.length) break;
        if (Date.now() >= end) throw new Error(`the Mini did not join the tailnet in 60 s: ${status.backendState} ${status.health?.join(' ') ?? ''}`);
        await new Promise(resolve => setTimeout(resolve, 250));
      }
      address = status.self.tailscaleIPs.find(ip => !ip.includes(':'));
      // A node that has just joined reaches the other nodes through the
      // control server a few seconds later; until this machine has it, a
      // connection to it waits for TCP's next retry.
      while (await path(address) === null) {
        if (Date.now() >= end) throw new Error(`this machine's Tailscale does not have ${address} 60 s after the Mini joined`);
        await new Promise(resolve => setTimeout(resolve, 250));
      }
      await page.goto(`http://${address}/`, { waitUntil: 'domcontentloaded' });
    },
    off: async () => {
      if (onTailnet()) await page.goto(url, { waitUntil: 'domcontentloaded' });
      address = null;
      pathNow = null;
      // A firmware without Tailscale answers that the method does not exist.
      await rpc('setTailscaleConfig', { enabled: false, authKey: '' }).catch(error => {
        if (!/not found/i.test(error.message)) throw error;
      });
    },
    // On: the Mini reports itself running on the tailnet, and the page is at
    // its tailnet address.
    isOn: async () => {
      const status = await rpc('getTailscaleStatus').catch(() => null);
      return Boolean(status?.running) && onTailnet();
    },
    // running: the Mini reports itself running on the tailnet, null without
    // an answer. address: its tailnet address. path: how this machine
    // reaches it, read every 5 seconds at most. onTailnet: the page is at
    // that address.
    sample: async () => {
      const status = await rpc('getTailscaleStatus', {}, 2000).catch(() => null);
      if (address && Date.now() - pathAt >= 5000) {
        pathAt = Date.now();
        pathNow = await path(address);
      }
      return { running: status ? Boolean(status.running) : null, address, path: pathNow, onTailnet: onTailnet() };
    },
  };
}
