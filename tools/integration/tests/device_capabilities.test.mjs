import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';

// In the page, before it loads: keeps each deviceCapabilities event of the rpc
// channel from the channel's creation. The Mini sends the event when the
// channel opens, before connect() listens.
function keepCapabilityEvents() {
  window.__integrationCapabilityEvents = [];
  const create = RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel = function (label, ...rest) {
    const channel = create.call(this, label, ...rest);
    if (label === 'rpc') {
      channel.addEventListener('message', event => {
        try {
          const message = JSON.parse(event.data);
          if (message.method === 'deviceCapabilities' && message.id === undefined) {
            window.__integrationCapabilityEvents.push(message.params);
          }
        } catch {
          // not JSON-RPC
        }
      });
    }
    return channel;
  };
}

// The page shows a setting only when the event names its capability; it does
// not call getDeviceCapabilities. A capability that only the answer has is a
// setting that the Mini takes and the page does not show.
test('the deviceCapabilities event of a new session has the capabilities that getDeviceCapabilities answers', { timeout: 120_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  await mini.page.addInitScript(keepCapabilityEvents);
  await mini.open();
  const events = await until(() => mini.page.evaluate(() => window.__integrationCapabilityEvents), kept => kept.length > 0, 10);
  const answer = await mini.rpc('getDeviceCapabilities');
  t.diagnostic(`event ${JSON.stringify(events[0])}; answer ${JSON.stringify(answer)}`);
  assert.deepEqual([...events[0]].sort(), [...answer].sort());
});
