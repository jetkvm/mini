// The tool's browser page and what it carries: the Mini's JSON-RPC, which
// goes over the page's WebRTC data channel. The walk (run.mjs, switches.mjs)
// and the integration tests (tools/integration) use the same page.
import { createRequire } from 'node:module';
import path from 'node:path';
import { inflateRawSync } from 'node:zlib';

/**
 * Starts the browser: Google Chrome (H.264 decoding), headless, with
 * playwright-core from frontend/jetkvm-ui/node_modules of the checkout at
 * root. Set PLAYWRIGHT_CHROMIUM_EXECUTABLE to use another Chromium build
 * with proprietary codecs.
 */
export function launchBrowser(root) {
  const require = createRequire(path.join(root, 'frontend', 'jetkvm-ui', 'node_modules') + path.sep);
  const { chromium } = require('playwright-core');
  return chromium.launch({
    headless: true,
    // The caller turns its switches off before the browser closes.
    handleSIGINT: false,
    handleSIGTERM: false,
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
      ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : { channel: 'chrome' }),
    // The page plays the Mini's sound, for audio, into Chrome's fake audio
    // output (--disable-audio-output), which takes the sound at the rate of
    // a real one and plays nothing: this machine stays silent, and its own
    // audio devices are no part of the measurement. With a real output,
    // Chrome stops taking the sound of the page's audio track for the rest
    // of the session when that output fails, as the USB audio device of a
    // Mini plugged into this machine does when that Mini restarts; the
    // browser then reports silence while the Mini's packets still arrive.
    // Video is decoded in software: a hardware decoder can show a picture
    // from a stream with missing reference pictures, which a software
    // decoder reports as dropped frames and requests for a keyframe.
    args: ['--autoplay-policy=no-user-gesture-required', '--disable-audio-output', '--disable-accelerated-video-decode'],
  });
}

/**
 * The hardware video decoders that the browser would use, as its system
 * information lists them (DevTools SystemInfo.getInfo): [] for a browser
 * that decodes video in software.
 */
export async function hardwareVideoDecoders(browser) {
  const session = await browser.newBrowserCDPSession();
  try {
    return (await session.send('SystemInfo.getInfo')).gpu?.videoDecoding ?? [];
  } finally {
    await session.detach().catch(() => undefined);
  }
}

/**
 * One call of the Mini's JSON-RPC through a page that shows the Mini, without
 * a wait for the RPC channel: call(method, params, timeoutMs) resolves with
 * the result and rejects on an error response, a timeout or a page without
 * the channel.
 */
export function pageCall(page) {
  return (method, params, timeoutMs) =>
    page.evaluate(([m, p, timeout]) => new Promise((resolve, reject) => {
      const hooks = window.__kvmTestHooks;
      if (!hooks) return reject(new Error(`${m}: the page has no RPC channel`));
      hooks.sendJsonRpc(m, p, r => (r.error
        ? reject(new Error(`${m}: ${r.error.message}${r.error.data ? `: ${r.error.data}` : ''}`))
        : resolve(r.result)), timeout);
    }), [method, params, timeoutMs]);
}

/**
 * The Mini's JSON-RPC through a page that shows the Mini:
 * rpc(method, params, timeoutMs) resolves with the result and rejects on an
 * error response or a timeout. The RPC channel opens shortly after video
 * starts; a call waits up to 15 seconds for it.
 */
export function pageRpc(page) {
  const call = pageCall(page);
  return async (method, params = {}, timeoutMs = 10_000) => {
    const end = Date.now() + 15_000;
    for (;;) {
      try {
        return await call(method, params, timeoutMs);
      } catch (e) {
        if (!/RPC data channel not available|has no RPC channel/.test(e.message) || Date.now() > end) throw e;
        await new Promise(resolve => setTimeout(resolve, 500));
      }
    }
  };
}

/** The diagnostics.json of the zip that /diagnostics answers with: its first entry. */
export function readDiagnostics(zip) {
  const start = 30 + zip.readUInt16LE(26) + zip.readUInt16LE(28);
  const data = zip.subarray(start, start + zip.readUInt32LE(18));
  return JSON.parse((zip.readUInt16LE(8) === 8 ? inflateRawSync(data) : data).toString('utf8'));
}

/** The Mini's /diagnostics, as a visitor without a login gets it. */
export async function diagnostics(url) {
  const response = await fetch(new URL('/diagnostics', url), { signal: AbortSignal.timeout(10_000) });
  return readDiagnostics(Buffer.from(await response.arrayBuffer()));
}
