import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';

// A browser that signals no ICE candidate (Firefox with
// media.peerconnection.ice.no_host, or a candidate that the Mini's address
// policy drops) still sends connectivity checks. The Mini knows such a
// browser only from the source address of its authenticated Binding Request.
test('a page that signals no ICE candidate gets a connection', { timeout: 180_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  const stats = async () => (await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(5000) })).json();
  // The first page must be gone, so that a connected peer is the page below.
  await mini.leave();
  const page = await mini.page.context().browser().newPage();
  let offers = 0;
  let removed = 0;
  try {
    await page.addInitScript(() => {
      const Original = window.RTCPeerConnection;
      window.__peers = [];
      window.RTCPeerConnection = class extends Original {
        constructor(...values) {
          super(...values);
          window.__peers.push(this);
        }
      };
    });
    await page.routeWebSocket(/\/webrtc\/signaling\/client/, socket => {
      const device = socket.connectToServer();
      socket.onMessage(message => {
        const signal = typeof message === 'string' && message.startsWith('{') ? JSON.parse(message) : null;
        if (signal?.type === 'new-ice-candidate') {
          removed++;
          return;
        }
        if (signal?.type !== 'offer') return device.send(message);
        const description = JSON.parse(Buffer.from(signal.data.sd, 'base64').toString());
        const lines = description.sdp.split('\r\n');
        const kept = lines.filter(line => !line.startsWith('a=candidate:') && line !== 'a=end-of-candidates');
        removed += lines.length - kept.length;
        offers++;
        const sd = Buffer.from(JSON.stringify({ ...description, sdp: kept.join('\r\n') })).toString('base64');
        device.send(JSON.stringify({ ...signal, data: { ...signal.data, sd } }));
      });
      device.onMessage(message => socket.send(message));
    });
    await until(() => page.goto(mini.url, { waitUntil: 'domcontentloaded', timeout: 10_000 }), () => true, 60);
    await until(() => offers, count => count > 0, 30);
    // With signaled candidates the connection takes a second or two. The page
    // gives up and sends a new offer after about 10 s; that offer has no
    // candidate either.
    const connected = await until(
      () => page.evaluate(() => (window.__peers ?? []).map(peer => peer.connectionState)),
      states => states.includes('connected'), 30).catch(() => null);
    t.diagnostic(`offers: ${offers}, candidates removed: ${removed}`);
    assert.ok(removed > 0, 'the page signaled no candidate of its own, so the test removed nothing');
    assert.ok(connected, 'the page that signaled no candidate did not connect in 30 s');
    assert.equal((await stats()).peerConnected, true);
  } finally {
    await page.close();
  }
});
