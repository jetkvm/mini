import assert from 'node:assert/strict';
import { once } from 'node:events';
import net from 'node:net';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { benchFile, readBench } from '../../combined_load/runner/build.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const bench = readBench(benchFile(root, process.env.INTEGRATION_BENCH ?? ''));
const { hostname } = new URL(bench.JETKVM_URL);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// Waits for the socket's event. A socket that does not emit it within `ms` is
// destroyed, and the wait fails with `message`. The wait also fails with the
// socket's error. No timer stays behind.
async function eventWithin(socket, event, ms, message) {
  const timer = setTimeout(() => socket.destroy(new Error(message)), ms);
  try {
    await once(socket, event);
  } finally {
    clearTimeout(timer);
  }
}

// One TCP connection to the Mini's port 80 from `localPort` (0: any), open.
// A socket that does not connect within `ms` is destroyed.
async function connectFrom(localPort, ms) {
  const socket = net.connect({ host: hostname, port: 80, localPort });
  // An error between two waits must not end the process; the next wait fails.
  socket.on('error', () => {});
  try {
    await eventWithin(socket, 'connect', ms, `port ${localPort}: no connection within ${ms} ms`);
  } catch (error) {
    socket.destroy();
    throw error;
  }
  return socket;
}

// The Mini closes the connection of the page's document (GET /) itself, so
// its side goes to TIME-WAIT. A browser then opens the signaling socket, and
// the host's kernel can give that connection the same source port. The Mini
// must take the new SYN; a Mini that answers it for the old connection keeps
// the browser waiting for 2 minutes (the page loads and stays disconnected).
// The limit of 5 s leaves room for the SYN retransmissions after 1 s and 3 s:
// one lost packet on Wi-Fi must not fail the test.
test('a new connection from the port of one that the Mini has just closed opens within 5 s', { timeout: 120_000 }, async t => {
  const times = [];
  for (let i = 0; i < 5; i++) {
    const first = await connectFrom(0, 10_000);
    let second;
    try {
      const port = first.localPort;
      first.resume();
      first.write(`GET / HTTP/1.1\r\nHost: ${hostname}\r\n\r\n`);
      await eventWithin(first, 'end', 10_000, 'the Mini did not close the connection of GET / within 10 s');
      first.end();
      await eventWithin(first, 'close', 10_000, `port ${port}: the first connection did not close within 10 s`);
      // The port is free on this side once the Mini has acknowledged the FIN.
      let startedAt;
      for (let attempt = 0; ; attempt++) {
        startedAt = Date.now();
        try {
          second = await connectFrom(port, 10_000);
          break;
        } catch (error) {
          if (!['EADDRINUSE', 'EADDRNOTAVAIL'].includes(error.code) || attempt >= 100) throw error;
          await sleep(20);
        }
      }
      times.push(Date.now() - startedAt);
    } finally {
      first.destroy();
      second?.destroy();
    }
  }
  t.diagnostic(`the second connections opened after ${times.join(', ')} ms`);
  assert.ok(Math.max(...times) < 5000, `each second connection opens within 5 s: ${times.join(', ')} ms`);
});
