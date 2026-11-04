// Helpers for tests that check the browser session survives a Mini change.
import { pageCall } from '../../combined_load/runner/page.mjs';
import { SOCKET_EVENTS, signalingSockets } from '../../combined_load/runner/takeover.mjs';

/**
 * Opens a new session and returns { sockets, peer, size }: sockets() counts
 * signaling WebSockets since then (1 = never reconnected); peer() reports
 * whether the RTCPeerConnection is the same and its state changes.
 */
export async function watchedSession(mini) {
  const { page } = mini;
  let signaling = signalingSockets();
  const devtools = await page.context().newCDPSession(page);
  for (const name of SOCKET_EVENTS) devtools.on(name, params => signaling.event(name, params, Date.now()));
  await devtools.send('Network.enable');
  // open() may retry a load; count only sockets of the document it ends on.
  let loading = true;
  devtools.on('Page.frameNavigated', ({ frame }) => {
    if (loading && !frame.parentId) signaling = signalingSockets();
  });
  await devtools.send('Page.enable');
  try {
    await mini.open();
  } finally {
    loading = false;
  }
  await page.evaluate(() => {
    const pc = window.__kvmTestHooks._getPeerConnection();
    const states = [];
    pc.addEventListener('connectionstatechange', () => states.push(pc.connectionState));
    window.__integrationPeer = { pc, states };
  });
  return {
    sockets: () => signaling.sockets().length,
    peer: () => page.evaluate(() => ({
      same: window.__kvmTestHooks._getPeerConnection() === window.__integrationPeer.pc,
      states: window.__integrationPeer.states,
    })),
    size: () => page.evaluate(() => window.__kvmTestHooks.getVideoStreamDimensions()),
  };
}

/**
 * On the next videoInputState ready=false, the page sends getDeviceID and
 * presses then releases modifier `key` ({ key, bit }); event-driven so short
 * stops are caught. Needs keyboard() installed. done() resolves with
 * { stoppedAt, rpcAt, rpcError, pressedAt, releasedAt, backAt } (null if missed).
 */
export async function whileVideoStopped(mini, key, { stopSeconds = 15, backSeconds = 45 } = {}) {
  await mini.page.evaluate(([usage, bit, stopMs, backMs]) => {
    const hooks = window.__kvmTestHooks;
    const keys = window.__integrationKeys;
    const times = { stoppedAt: null, rpcAt: null, rpcError: null, pressedAt: null, releasedAt: null, backAt: null };
    const rpc = hooks._getRpcDataChannel();
    window.__integrationStop = new Promise(resolve => {
      let timer = setTimeout(() => finish(), stopMs);
      const onReport = event => {
        const bytes = new Uint8Array(event.data);
        if (bytes[0] !== 0x33 || times.stoppedAt === null) return;
        const down = (bytes[1] & bit) !== 0;
        if (down && times.pressedAt === null) {
          times.pressedAt = Date.now();
          keys.press(usage, false);
        } else if (!down && times.pressedAt !== null && times.releasedAt === null) {
          times.releasedAt = Date.now();
          check();
        }
      };
      const onMessage = event => {
        let message;
        try {
          message = JSON.parse(event.data);
        } catch {
          return;
        }
        if (message.method !== 'videoInputState' || message.id !== undefined) return;
        if (!message.params?.ready && times.stoppedAt === null) {
          times.stoppedAt = Date.now();
          clearTimeout(timer);
          timer = setTimeout(() => finish(), backMs);
          hooks.sendJsonRpc('getDeviceID', {}, answer => {
            times.rpcAt = Date.now();
            times.rpcError = answer.error ? answer.error.message : null;
            check();
          }, 5000);
          keys.press(usage, true);
        } else if (message.params?.ready && times.stoppedAt !== null && times.backAt === null) {
          times.backAt = Date.now();
          check();
        }
      };
      const finish = () => {
        clearTimeout(timer);
        rpc.removeEventListener('message', onMessage);
        keys.channel.removeEventListener('message', onReport);
        resolve(times);
      };
      const check = () => {
        if (times.backAt !== null && times.rpcAt !== null && times.releasedAt !== null) finish();
      };
      rpc.addEventListener('message', onMessage);
      keys.channel.addEventListener('message', onReport);
    });
  }, [key.key, key.bit, stopSeconds * 1000, backSeconds * 1000]);
  return { done: () => mini.page.evaluate(() => window.__integrationStop) };
}

// Calls getDeviceID (no retries) 100 ms after each answer until stop(), which
// resolves with { sent, failures }.
export function rpcEvery100ms(mini) {
  const call = pageCall(mini.page);
  const failures = [];
  let sent = 0;
  let running = true;
  const loop = (async () => {
    while (running) {
      sent++;
      await call('getDeviceID', {}, 5000).catch(error => failures.push(error.message));
      await new Promise(resolve => setTimeout(resolve, 100));
    }
  })();
  return {
    stop: async () => {
      running = false;
      await loop;
      return { sent, failures };
    },
  };
}
