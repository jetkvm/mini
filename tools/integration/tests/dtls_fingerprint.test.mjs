import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';

// An offer whose a=fingerprint does not match the DTLS certificate must be
// refused at the handshake.
const OTHER = Array.from({ length: 32 }, (_, i) => (i * 7 + 3).toString(16).padStart(2, '0').toUpperCase()).join(':');

test('a page whose offer names another certificate gets no connection, and the next page gets one', { timeout: 180_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  const stats = async () => (await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(5000) })).json();
  const page = await mini.page.context().browser().newPage();
  let offers = 0;
  try {
    await page.routeWebSocket(/\/webrtc\/signaling\/client/, socket => {
      const device = socket.connectToServer();
      socket.onMessage(message => {
        const signal = typeof message === 'string' ? JSON.parse(message) : null;
        if (signal?.type !== 'offer') return device.send(message);
        const description = JSON.parse(Buffer.from(signal.data.sd, 'base64').toString());
        const sdp = description.sdp.replace(/^a=fingerprint:sha-256 [0-9A-F:]+(?=\r?$)/gim, `a=fingerprint:sha-256 ${OTHER}`);
        if (sdp !== description.sdp) offers++;
        const sd = Buffer.from(JSON.stringify({ ...description, sdp })).toString('base64');
        device.send(JSON.stringify({ ...signal, data: { ...signal.data, sd } }));
      });
      device.onMessage(message => socket.send(message));
    });
    await until(() => page.goto(mini.url, { waitUntil: 'domcontentloaded', timeout: 10_000 }), () => true, 60);
    await until(() => offers, count => count > 0, 30);
    // The handshake of a matching certificate takes a second or two.
    const end = Date.now() + 20_000;
    while (Date.now() < end) {
      assert.equal((await stats()).peerConnected, false, 'the Mini connected a peer whose certificate is not the one of the offer');
      await new Promise(resolve => setTimeout(resolve, 250));
    }
  } finally {
    await page.close();
  }
  t.diagnostic(`offers with another fingerprint: ${offers}`);
  await mini.open();
  assert.equal((await stats()).peerConnected, true);
  assert.equal(typeof await mini.rpc('getDeviceID'), 'string');
});
