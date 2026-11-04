import assert from 'node:assert/strict';
import { after, before, describe, test } from 'node:test';
import { seededBytes, sha256 } from '../../combined_load/runner/content.mjs';
import { checksumIn, quote } from '../../combined_load/runner/hosts.mjs';
import { openChannel, sendFiles } from '../lib/channel.mjs';
import { connect, until } from '../lib/mini.mjs';
import { openSession, rpcAnswer, videoState } from '../lib/os_services.mjs';
import { senderHost } from '../lib/sender_host.mjs';

// The file transfer of OS Services, as the page does it for files that its
// user drops on the video: guestFileDrop names a channel and one transfer
// per file, the Mini gives credit in bytes on that channel, and reports each
// file's path on the host when the sender has written it.
describe('the OS Services file transfer', () => {
  let mini;
  let host;
  let session;
  // The files that the sender has written, which the test removes.
  const written = [];

  before(async () => {
    mini = await connect();
    try {
      host = await senderHost(mini);
      session = await openSession(mini, host);
      if (!(await mini.rpc('getOsServicesSettings')).files_enabled) {
        await mini.rpc('setOsServicesSettings', { settings: { files_enabled: true } });
      }
      await until(() => videoState(mini), state => state.osServices.connected && state.guestFile?.receive === true, 45);
    } catch (error) {
      await session?.restore().catch(() => undefined);
      await mini.close().catch(() => undefined);
      throw error;
    }
  }, { timeout: 300_000 });

  after(async () => {
    try {
      for (const path of written) await host.exec(`rm -f -- ${quote(path)}`).catch(() => undefined);
      await session?.restore();
    } finally {
      await mini.close();
    }
  }, { timeout: 300_000 });

  // Asks the Mini for a drop of these files, { name, size, seed, cancelAfter },
  // and sends them. Returns done(seconds): the state of the transfers when
  // the Mini has reported each outcome and the channel has closed.
  const drop = async files => {
    const answer = await mini.rpc('guestFileDrop', { drop: { files: files.map(({ name, size }) => ({ name, size })) } });
    assert.match(answer.channel, /^guestfile_\d+$/, 'the channel of the drop');
    const id = Number(answer.channel.slice('guestfile_'.length));
    assert.deepEqual(answer.transfers, files.map((_, index) => id + 1 + index), 'one transfer per file, numbered after the drop');
    const state = await sendFiles(mini, answer.channel, files.map((file, index) => ({ ...file, transfer: answer.transfers[index] })));
    return async seconds => {
      const end = await until(state, now => now.transfers.every(transfer => transfer.status) && now.readyState === 'closed', seconds);
      written.push(...end.transfers.map(transfer => transfer.path).filter(Boolean));
      return end;
    };
  };

  test('a drop writes each file byte for byte, a cancelled transfer reports "cancelled", and an unknown drop gets no answer', { timeout: 420_000 }, async t => {
    const stamp = Date.now();
    const files = [
      { name: `integration-${stamp}-small.txt`, size: 5, seed: 1 },
      { name: `integration-${stamp}-big.bin`, size: 4 * 1024 * 1024 + 123, seed: 2 },
      { name: `integration-${stamp}-empty.bin`, size: 0, seed: 3 },
    ];
    const started = Date.now();
    const sent = await (await drop(files))(120);
    const seconds = (Date.now() - started) / 1000;
    assert.deepEqual(sent.other, [], 'only credit and done messages');
    for (const [index, file] of files.entries()) {
      const transfer = sent.transfers[index];
      assert.equal(transfer.status, 'ok', `${file.name}: status`);
      assert.equal(transfer.sent, file.size, `${file.name}: bytes sent`);
      // The credit does not go past the end of the file.
      assert.equal(transfer.credit, file.size, `${file.name}: credit`);
      // A path of the host, with the separator of its system.
      assert.match(transfer.path ?? '', /[\\/]/, `${file.name}: the path that the sender wrote`);
      assert.equal(checksumIn(await host.exec(host.commands.checksumsAndRemove([transfer.path])), transfer.path), sha256(seededBytes(file.seed, file.size)),
        `${file.name}: SHA-256 on the host`);
    }
    const total = files.reduce((sum, file) => sum + file.size, 0);
    t.diagnostic(`${total} bytes in ${seconds.toFixed(1)} s, ${Math.round(total / seconds)} bytes per second, ` +
      `${sent.transfers.map(transfer => transfer.creditMessages).join('/')} credit messages`);
    assert.ok(total / seconds > 200_000, `${Math.round(total / seconds)} bytes per second`);

    // A transfer of 16 MiB that the page cancels after 1 MiB.
    const size = 16 * 1024 * 1024;
    const cancelled = await drop([{ name: `integration-${stamp}-cancelled.bin`, size, seed: 4, cancelAfter: 1024 * 1024 }]);
    const second = await rpcAnswer(mini, 'guestFileDrop', { drop: { files: [{ name: `integration-${stamp}-second.txt`, size: 1 }] } });
    assert.equal(second.error?.data, 'a file transfer is already in progress', 'a second drop during the first');
    const [transfer] = (await cancelled(60)).transfers;
    assert.equal(transfer.status, 'cancelled');
    assert.ok(transfer.sent < size, `sending stopped after ${transfer.sent} bytes`);
    // The next drop starts when the cancelled one is over.
    const next = await (await drop([{ name: `integration-${stamp}-after.txt`, size: 3, seed: 5 }]))(30);
    assert.equal(next.transfers[0].status, 'ok', 'the drop after the cancelled one');

    // A channel for a drop that the Mini did not name.
    const unknown = await openChannel(mini, 'guestfile_4000000000');
    const closed = await unknown.until(state => state.readyState === 'closed', 20);
    assert.deepEqual(closed.texts, [], 'no message');
    assert.equal(closed.binaryMessages, 0, 'no output');
  });
});
