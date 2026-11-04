import assert from 'node:assert/strict';
import { connect as connectTcp } from 'node:net';
import { test } from 'node:test';
import { setTimeout as sleep } from 'node:timers/promises';
import { imageBytes, sha256 } from '../../combined_load/runner/content.mjs';
import { connectHost, hostReadsImage } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';

// The Mini writes whole 24 KiB blocks: a cut-off upload keeps those as
// <name>.incomplete, and the next start resumes from their size.
const FILE_BLOCKS = 16;
const FILE_SIZE = FILE_BLOCKS * 1024 * 1024;
const PIPELINE_BLOCK = 24 * 1024;
// Not at the end of a block: 171 blocks and 4153 bytes.
const CUT_AT = 4 * 1024 * 1024 + 12_345;
const COMMITTED = Math.floor(CUT_AT / PIPELINE_BLOCK) * PIPELINE_BLOCK;
const SEND_CHUNK = 16 * 1024;
const BUSY = /another virtual media operation is active/;

// The files of this run on the card, as the Mini lists them.
async function entries(mini, name) {
  const { files } = await mini.rpc('listStorageFiles');
  return files.filter(file => file.filename === name || file.filename === `${name}.incomplete`)
    .map(({ filename, size }) => ({ filename, size }));
}

// The start call, again while the Mini still has the writer of the POST
// before: the bytes on the card are settled when a start answers with them.
async function startWhenFree(mini, params, seconds) {
  const end = Date.now() + seconds * 1000;
  for (;;) {
    try {
      return await mini.rpc('startStorageFileUpload', params);
    } catch (error) {
      if (!BUSY.test(error.message) || Date.now() >= end) throw error;
    }
    await sleep(250);
  }
}

const uploadUrl = (mini, uploadId) => new URL(`/storage/upload?uploadId=${encodeURIComponent(uploadId)}`, mini.url);

// Opens a POST that announces the whole of `data` and sends its first
// `length` bytes. Returns the connection, which is still open.
async function sendFirstPart(mini, uploadId, data, length) {
  const url = uploadUrl(mini, uploadId);
  const socket = connectTcp({ host: url.hostname, port: Number(url.port || 80) });
  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });
  // The Mini's answer is read and dropped, so that the connection can end.
  socket.resume();
  const write = chunk => new Promise((resolve, reject) => socket.write(chunk, error => (error ? reject(error) : resolve())));
  await write(`POST ${url.pathname}${url.search} HTTP/1.1\r\nHost: ${url.host}\r\nContent-Type: application/octet-stream\r\n` +
    `Content-Length: ${data.length}\r\nConnection: close\r\n\r\n`);
  for (let offset = 0; offset < length; offset += SEND_CHUNK) await write(data.subarray(offset, Math.min(offset + SEND_CHUNK, length)));
  return socket;
}

// One POST of `body` to an upload. Returns the status and the answer's text.
async function post(mini, uploadId, body, seconds) {
  const response = await fetch(uploadUrl(mini, uploadId), {
    method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body, signal: AbortSignal.timeout(seconds * 1000),
  });
  return { status: response.status, text: await response.text() };
}

// usbHid.uploadWrite of /webrtc/stats: the Mini's counts of its writes of
// uploaded bytes to the card.
async function uploadWrite(mini) {
  const stats = await (await fetch(new URL('/webrtc/stats', mini.url), { signal: AbortSignal.timeout(10_000) })).json();
  return stats.usbHid.uploadWrite;
}

// The Mini and the bench's host for a test with the file `name`, which is of
// this run alone. The cleanup ends a POST that is still open and deletes
// what the test left of the file; the Mini deletes nothing while a writer of
// the file is at work, so the cleanup asks until the files are gone.
async function bench(t, name) {
  const mini = await connect();
  const host = await connectHost(mini).catch(async error => {
    await mini.close();
    throw error;
  });
  if (host.commands.releaseMedia) {
    await mini.close();
    t.skip(`the test reads the card through a disk on the host, and ${host.login} is a ${host.system} host, which keeps a disk that the Mini unmounts`);
    return null;
  }
  const open = { socket: null };
  t.after(async () => {
    try {
      open.socket?.destroy();
      await until(async () => {
        const left = await entries(mini, name);
        for (const { filename } of left) await mini.rpc('deleteStorageFile', { filename });
        return left;
      }, left => left.length === 0, 60);
    } finally {
      await mini.close();
    }
  });
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'the Mini has no image mounted before the test');
  return { mini, host, open };
}

// After the cut: a start for the same file answers with the whole blocks of
// what was sent, and the card has exactly that much as the partial file.
async function startAfterCut(mini, name, params) {
  const resumed = await startWhenFree(mini, params, 20);
  assert.equal(resumed.alreadyUploadedBytes, COMMITTED, 'the start after the cut answers with the whole 24 KiB blocks that were sent, not with 0');
  assert.deepEqual(await entries(mini, name), [{ filename: `${name}.incomplete`, size: COMMITTED }], 'the card has the partial file alone, with those blocks');
  return resumed;
}

// The rest of the file, from the byte that the start named, and what the
// card has then.
async function sendRest(mini, name, data, resumed, seconds) {
  const rest = await post(mini, resumed.dataChannel, data.subarray(resumed.alreadyUploadedBytes), seconds);
  assert.equal(rest.status, 200, rest.text);
  assert.equal(JSON.parse(rest.text).bytes, FILE_SIZE - COMMITTED, 'the Mini took the rest of the file and no more');
  assert.deepEqual(await entries(mini, name), [{ filename: name, size: FILE_SIZE }], 'the card has the finished file, and no partial file');
}

// The host reads the file from the card, and every block is the one that
// was sent.
async function hostReadsFile({ mini, host }, name, data) {
  const { blocksWrong, readErrors } = await hostReadsImage(mini, host, name, data);
  assert.deepEqual({ blocksWrong, readErrors }, { blocksWrong: 0, readErrors: 0 }, 'the host reads the file as it was sent');
}

// The page's Cancel Upload ends the POST in the middle of the file, and the
// page starts the upload again without a checksum of the file.
test('an upload whose connection the client closes resumes from the committed blocks', { timeout: 600_000 }, async t => {
  const name = `integration-upload-closed-${Date.now()}.img`;
  const found = await bench(t, name);
  if (!found) return;
  const { mini, open } = found;
  const data = imageBytes(Math.floor(Date.now() / 1000), FILE_BLOCKS);
  const params = { filename: name, size: FILE_SIZE };

  const first = await mini.rpc('startStorageFileUpload', params);
  assert.equal(first.alreadyUploadedBytes, 0, 'a new upload starts at 0');
  open.socket = await sendFirstPart(mini, first.dataChannel, data, CUT_AT);
  // The end of the client's side arrives after the bytes, and the Mini then
  // ends its side.
  open.socket.end();
  await until(() => open.socket.destroyed, Boolean, 20);

  const resumed = await startAfterCut(mini, name, params);
  await sendRest(mini, name, data, resumed, 40);
  await hostReadsFile(found, name, data);
});

test('an upload cut by a TCP reset resumes from the committed blocks', { timeout: 600_000 }, async t => {
  const name = `integration-upload-reset-${Date.now()}.img`;
  const found = await bench(t, name);
  if (!found) return;
  const { mini, open } = found;
  const data = imageBytes(Math.floor(Date.now() / 1000), FILE_BLOCKS);
  const params = { filename: name, size: FILE_SIZE, sha256: sha256(data) };

  const first = await mini.rpc('startStorageFileUpload', params);
  assert.equal(first.alreadyUploadedBytes, 0, 'a new upload starts at 0');
  const written = (await uploadWrite(mini)).bytesWritten;
  open.socket = await sendFirstPart(mini, first.dataChannel, data, CUT_AT);
  // A reset can overtake bytes that are on their way, so it comes when the
  // Mini has written every whole block of what was sent.
  await until(() => uploadWrite(mini), now => now.bytesWritten - written >= COMMITTED, 20);
  open.socket.resetAndDestroy();

  await startAfterCut(mini, name, params);
  const other = params.sha256.replace(/^./, digit => (digit === '0' ? '1' : '0'));
  await assert.rejects(mini.rpc('startStorageFileUpload', { ...params, sha256: other }), 'a start with another SHA-256 is refused');
  const resumed = await mini.rpc('startStorageFileUpload', params);
  assert.equal(resumed.alreadyUploadedBytes, COMMITTED, 'the refused start left the partial file');
  assert.deepEqual(await entries(mini, name), [{ filename: `${name}.incomplete`, size: COMMITTED }]);

  const before = await uploadWrite(mini);
  await sendRest(mini, name, data, resumed, 120);
  // The rest went to the card in blocks of 24 KiB, each written straight
  // from the buffer that received it: no copy and no staging buffer.
  const after = await uploadWrite(mini);
  const tail = FILE_SIZE - COMMITTED;
  const blocks = Math.ceil(tail / PIPELINE_BLOCK);
  const change = Object.fromEntries(['calls', 'bytesRequested', 'bytesWritten', 'errors', 'directCalls', 'directBytes', 'bounceCalls',
    'bounceBytes', 'stagingAllocations', 'stagingAllocationFailures', 'stagingReleases'].map(key => [key, after[key] - before[key]]));
  assert.deepEqual({ ...change, stagingBytes: after.stagingBytes, stagingActive: after.stagingActive }, {
    calls: blocks, bytesRequested: tail, bytesWritten: tail, errors: 0, directCalls: blocks, directBytes: tail, bounceCalls: 0,
    bounceBytes: 0, stagingAllocations: 0, stagingAllocationFailures: 0, stagingReleases: 0, stagingBytes: 0, stagingActive: false,
  });
  await hostReadsFile(found, name, data);
});

// The page's Cancel Upload before the start's answer leaves an upload that
// gets no POST, and the next choice of the file starts it again. The Mini
// ends the upload before when one starts for the same file: the new start
// gets an upload ID of its own, and the file goes to that one.
test('a second start for the same file supersedes the first', { timeout: 600_000 }, async t => {
  const name = `integration-upload-supersede-${Date.now()}.img`;
  const found = await bench(t, name);
  if (!found) return;
  const { mini } = found;
  const data = imageBytes(Math.floor(Date.now() / 1000), 1);
  const params = { filename: name, size: data.length };

  const first = await mini.rpc('startStorageFileUpload', params);
  const second = await mini.rpc('startStorageFileUpload', params);
  assert.notEqual(second.dataChannel, first.dataChannel, 'a new start gives a new upload ID');
  const whole = await post(mini, second.dataChannel, data, 30);
  assert.equal(whole.status, 200, whole.text);
  await hostReadsFile(found, name, data);
});
