// Keyboard and pointer over the page's `hidrpc` channel.
import { until } from './mini.mjs';

// Runs in the page. Mirrors src/hooks/useKeyboard.ts: keypress 0x05, keepalive
// 0x09 every 50 ms while any key is held, key-state report 0x33.
function installKeyboard() {
  const channel = window.__integrationChannels?.hidrpc;
  if (channel?.readyState !== 'open' || !window.__kvmTestHooks?.isHidRpcReady()) return false;
  if (window.__integrationKeys?.channel === channel) return true;
  const KEEPALIVE_INTERVAL = 50;
  const held = new Set();
  let timer = null;
  let down = { modifier: 0, keys: [0, 0, 0, 0, 0, 0] };
  const cancelKeepAlive = () => {
    if (timer) {
      clearInterval(timer);
      timer = null;
    }
  };
  const scheduleKeepAlive = () => {
    cancelKeepAlive();
    timer = setInterval(() => channel.send(new Uint8Array([0x09])), KEEPALIVE_INTERVAL);
  };
  channel.addEventListener('message', event => {
    const bytes = new Uint8Array(event.data);
    if (bytes[0] === 0x33) down = { modifier: bytes[1], keys: Array.from(bytes.slice(2)) };
  });
  window.__integrationKeys = {
    channel,
    press: (key, press) => {
      if (press) held.add(key);
      else held.delete(key);
      channel.send(new Uint8Array([0x05, key, press ? 1 : 0]));
      if (held.size > 0) {
        if (!timer) scheduleKeepAlive();
      } else {
        cancelKeepAlive();
      }
    },
    pause: ms => {
      cancelKeepAlive();
      setTimeout(() => {
        if (held.size > 0) scheduleKeepAlive();
      }, ms);
    },
    down: () => down,
    held: () => [...held],
    // 0x03, x and y as big-endian u32, buttons.
    pointer: (x, y, buttons) => channel.send(new Uint8Array(
      [0x03, x >>> 24, (x >>> 16) & 0xff, (x >>> 8) & 0xff, x & 0xff, y >>> 24, (y >>> 16) & 0xff, (y >>> 8) & 0xff, y & 0xff, buttons])),
  };
  window.__integrationKeys.sequence = async steps => {
    for (const [key, press, ms] of steps) {
      window.__integrationKeys.press(key, press);
      await new Promise(resolve => setTimeout(resolve, ms));
    }
  };
  return true;
}

/**
 * { press, sequence, pause, down, releaseAll, pointer } for `mini` or a
 * mini.session(). sequence([[key, press, ms], ...]) is timed in the page.
 * pointer coordinates are 0..32767.
 */
export async function keyboard(mini) {
  await until(() => mini.page.evaluate(installKeyboard), Boolean, 30);
  return {
    press: (key, press) => mini.page.evaluate(([k, p]) => window.__integrationKeys.press(k, p), [key, press]),
    sequence: steps => mini.page.evaluate(list => window.__integrationKeys.sequence(list), steps),
    pause: ms => mini.page.evaluate(duration => window.__integrationKeys.pause(duration), ms),
    pointer: (x, y, buttons) => mini.page.evaluate(([px, py, b]) => window.__integrationKeys.pointer(px, py, b), [x, y, buttons]),
    down: () => mini.page.evaluate(() => window.__integrationKeys.down()),
    releaseAll: () => mini.page.evaluate(() => {
      for (const key of window.__integrationKeys.held()) window.__integrationKeys.press(key, false);
    }),
  };
}

// Taps space until hostAgent `host` reports a press; [] after `seconds`.
export async function spaceReachesHost(mini, host, seconds) {
  const SPACE = 0x2c;
  const KEY_SPACE = 57;
  const end = Date.now() + seconds * 1000;
  while (Date.now() < end) {
    try {
      const keys = await keyboard(mini);
      await host.clear('/events/keyboard');
      await keys.press(SPACE, true);
      await new Promise(resolve => setTimeout(resolve, 20));
      await keys.press(SPACE, false);
      return await until(
        async () => (await host.get('/events/keyboard')).filter(event => event.code === KEY_SPACE && event.type === 'key_press'),
        events => events.length > 0, 3);
    } catch {
      // The host may list the keyboard before it can open it.
    }
  }
  return [];
}
