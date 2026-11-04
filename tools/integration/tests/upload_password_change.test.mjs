import assert from 'node:assert/strict';
import { connect as connectTcp } from 'node:net';
import { test } from 'node:test';
import { setTimeout as sleep } from 'node:timers/promises';
import { imageBytes } from '../../combined_load/runner/content.mjs';
import { connect, until } from '../lib/mini.mjs';
import { webrtcStats } from '../lib/video.mjs';

const PASSWORD = 'UploadPassword123';
const FILE_BLOCKS = 64;
const FILE_SIZE = FILE_BLOCKS * 1024 * 1024;
const SEND_CHUNK = 16 * 1024;
// The upload is at work on the card when it has written this much.
const WRITING = 4 * 1024 * 1024;

// usbHid.uploadWrite of /webrtc/stats: the Mini's counts of its writes of
// uploaded bytes to the card.
const uploadWrite = async mini => (await webrtcStats(mini)).usbHid.uploadWrite;

// Card writes must stop before the password change answers, and the partial
// file must hold exactly the counted bytes.
test('a password change during an upload ends it, and no card write follows the answer', { timeout: 300_000 }, async t => {
  const mini = await connect();
  const name = `integration-upload-password-${Date.now()}.img`;
  const post = (pathname, body, method = 'POST') => mini.http(pathname, { method, body });
  const visitor = async () => (await fetch(new URL('/device', mini.url), { signal: AbortSignal.timeout(10_000) })).status;
  const sending = { socket: null, stopped: false };
  // Set before the first call that changes the password.
  let changed = false;
  t.after(async () => {
    try {
      sending.stopped = true;
      sending.socket?.destroy();
      if (changed && await visitor() !== 200) {
        assert.equal(await post('/auth/login-local', { password: PASSWORD }), 200);
        assert.equal(await post('/auth/local-password', { password: PASSWORD }, 'DELETE'), 200, 'the test\'s password is removed again');
        await mini.open();
      }
      await until(async () => {
        const { files } = await mini.rpc('listStorageFiles');
        const left = files.filter(file => file.filename.startsWith(name)).map(file => file.filename);
        for (const filename of left) await mini.rpc('deleteStorageFile', { filename });
        return left;
      }, left => left.length === 0, 60);
    } finally {
      await mini.close();
    }
  });
  assert.equal(await visitor(), 200, 'the bench\'s Mini has no password');

  const data = imageBytes(Math.floor(Date.now() / 1000), FILE_BLOCKS);
  const { dataChannel: uploadId } = await mini.rpc('startStorageFileUpload', { filename: name, size: FILE_SIZE });
  const before = await uploadWrite(mini);

  // The POST of the whole file, from a client without a cookie, as a page
  // that the password change logs out. It sends until the Mini closes the
  // connection.
  const url = new URL(`/storage/upload?uploadId=${encodeURIComponent(uploadId)}`, mini.url);
  sending.socket = connectTcp({ host: url.hostname, port: Number(url.port || 80) });
  await new Promise((resolve, reject) => {
    sending.socket.once('connect', resolve);
    sending.socket.once('error', reject);
  });
  sending.socket.on('error', () => {});
  sending.socket.resume();
  const write = chunk => new Promise(resolve => sending.socket.write(chunk, resolve));
  const sent = (async () => {
    await write(`POST ${url.pathname}${url.search} HTTP/1.1\r\nHost: ${url.host}\r\nContent-Type: application/octet-stream\r\n` +
      `Content-Length: ${FILE_SIZE}\r\nConnection: close\r\n\r\n`);
    for (let offset = 0; offset < FILE_SIZE && !sending.stopped && !sending.socket.destroyed; offset += SEND_CHUNK) {
      await write(data.subarray(offset, offset + SEND_CHUNK));
    }
  })();
  await until(() => uploadWrite(mini), now => now.bytesWritten - before.bytesWritten >= WRITING, 60);

  changed = true;
  const asked = Date.now();
  assert.equal(await post('/auth/password-local', { password: PASSWORD }), 201, 'the Mini takes a password during the upload');
  const answeredMs = Date.now() - asked;
  const atAnswer = await uploadWrite(mini);
  const written = atAnswer.bytesWritten - before.bytesWritten;
  assert.ok(written < FILE_SIZE, `the upload had not ended at the password change (${written} bytes written)`);

  // The client sends on for 3 seconds, unless the Mini has closed the
  // connection before.
  await sleep(3000);
  const later = await uploadWrite(mini);
  assert.deepEqual({ calls: later.calls, bytesWritten: later.bytesWritten },
    { calls: atAnswer.calls, bytesWritten: atAnswer.bytesWritten },
    `no card write after the password change answered (it answered after ${answeredMs} ms)`);
  await until(() => sending.socket.destroyed || sending.socket.readableEnded, Boolean, 30);
  sending.stopped = true;
  await sent;

  // The browser that set the password is logged in, and removes it.
  assert.equal(await post('/auth/local-password', { password: PASSWORD }, 'DELETE'), 200);
  changed = false;
  await mini.open();
  const { files } = await mini.rpc('listStorageFiles');
  assert.deepEqual(files.filter(file => file.filename.startsWith(name)).map(({ filename, size }) => ({ filename, size })),
    [{ filename: `${name}.incomplete`, size: written }], 'the card has the partial file with the bytes that the Mini counted');
});
