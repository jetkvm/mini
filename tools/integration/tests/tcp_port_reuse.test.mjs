import assert from 'node:assert/strict';
import net from 'node:net';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { benchFile, readBench } from '../../combined_load/runner/build.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const bench = readBench(benchFile(root, process.env.INTEGRATION_BENCH ?? ''));
const { hostname } = new URL(bench.JETKVM_URL);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// One TCP connection to the Mini's port 80 from `localPort` (0: any), open.
const connectFrom = localPort => new Promise((resolve, reject) => {
  const socket = net.connect({ host: hostname, port: 80, localPort });
  socket.once('connect', () => resolve(socket));
  socket.once('error', reject);
});

// The Mini closes the connection of the page's document (GET /) itself, so
// its side goes to TIME-WAIT. A browser then opens the signaling socket, and
// the host's kernel can give that connection the same source port. The Mini
// must take the new SYN; a Mini that answers it for the old connection keeps
// the browser waiting for 2 minutes (the page loads and stays disconnected).
test('a new connection from the port of one that the Mini has just closed opens at once', { timeout: 120_000 }, async t => {
  const times = [];
  for (let i = 0; i < 5; i++) {
    const first = await connectFrom(0);
    const port = first.localPort;
    const closedByMini = new Promise(resolve => first.once('end', resolve));
    first.resume();
    first.write(`GET / HTTP/1.1\r\nHost: ${hostname}\r\n\r\n`);
    await Promise.race([closedByMini, sleep(10_000).then(() => assert.fail('the Mini did not close the connection of GET /'))]);
    await new Promise(resolve => { first.once('close', resolve); first.end(); });
    // The port is free on this side once the Mini has acknowledged the FIN.
    let second, startedAt;
    for (let attempt = 0; ; attempt++) {
      startedAt = Date.now();
      try {
        second = await Promise.race([connectFrom(port), sleep(10_000).then(() => null)]);
        break;
      } catch (error) {
        if (!['EADDRINUSE', 'EADDRNOTAVAIL'].includes(error.code) || attempt >= 100) throw error;
        await sleep(20);
      }
    }
    assert.ok(second, `port ${port}: no connection within 10 s of the Mini's close`);
    times.push(Date.now() - startedAt);
    second.destroy();
  }
  t.diagnostic(`the second connections opened after ${times.join(', ')} ms`);
  assert.ok(Math.max(...times) < 1000, `each second connection opens within 1 s: ${times.join(', ')} ms`);
});
