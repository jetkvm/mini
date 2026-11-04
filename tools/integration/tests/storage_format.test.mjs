import assert from 'node:assert/strict';
import { connect as connectTcp } from 'node:net';
import { test } from 'node:test';
import { seededBytes, sha256 } from '../../combined_load/runner/content.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';
import { webrtcStats } from '../lib/video.mjs';

// formatStorage erases the TF card: every file on it is gone after these
// tests. The bench's tools put back what they need: the walk uploads its
// image at its start, and each test uploads its own files.
const SIZE = 1024 * 1024;
const FORMAT_MS = 120_000;
const IMAGE_MOUNTED = /An image is mounted\. Unmount it, then format the card\./;
const UPLOAD_ACTIVE = /A file is being written to the TF card/;

// The Mini with nothing mounted, and the USB devices of the host tests, so
// that a mount from the card reaches a drive. name(label) is a file name of
// this test alone; the cleanup runs what the test put into `undo`, unmounts,
// deletes the test's files, stores the USB state as found and closes.
async function bench(t) {
  const mini = await connect();
  const stamp = String(Date.now());
  const undo = [];
  let restoreUsb = null;
  t.after(async () => {
    try {
      for (const step of undo) await step();
      await mini.rpc('unmountImage').catch(() => undefined);
      // The Mini deletes no file that an upload still writes.
      await until(async () => {
        const left = (await files(mini)).filter(file => file.filename.includes(stamp)).map(file => file.filename);
        for (const filename of left) await mini.rpc('deleteStorageFile', { filename });
        return left;
      }, left => left.length === 0, 60);
      await restoreUsb?.();
    } finally {
      await mini.close();
    }
  });
  restoreUsb = await hostTestUsb(mini);
  return { mini, undo, name: label => `integration-format-${label}-${stamp}.img` };
}

// One whole upload: the start call and one POST of the bytes.
async function upload(mini, filename, data) {
  const { dataChannel } = await mini.rpc('startStorageFileUpload', { filename, size: data.length, sha256: sha256(data) });
  const response = await fetch(new URL(`/storage/upload?uploadId=${encodeURIComponent(dataChannel)}`, mini.url), {
    method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: data, signal: AbortSignal.timeout(60_000),
  });
  assert.equal(response.status, 200, await response.text());
}

const files = async mini => (await mini.rpc('listStorageFiles')).files.map(({ filename, size }) => ({ filename, size }));

// A mount and an unmount of an image from the card, as the UI's storage view does.
async function mountsFromCard(mini, filename) {
  await mini.rpc('mountWithStorage', { filename, mode: 'Disk' }, 60_000);
  const state = await mini.rpc('getVirtualMediaState');
  assert.deepEqual({ source: state?.source, filename: state?.filename, size: state?.size },
    { source: 'Storage', filename, size: SIZE }, 'the image from the card is mounted');
  await mini.rpc('unmountImage');
}

test('a format erases the card and leaves an empty exFAT card that takes an upload and a mount', { timeout: 300_000 }, async t => {
  const { mini, name } = await bench(t);
  const before = name('before');
  const after = name('after');
  await upload(mini, before, seededBytes(1, SIZE));
  assert.ok((await files(mini)).some(file => file.filename === before), 'the card has a file before the format');

  const formatted = await mini.rpc('formatStorage', {}, FORMAT_MS);
  t.diagnostic(`after the format: ${JSON.stringify(formatted)}`);
  assert.ok(formatted.bytesFree > 0, 'the new filesystem has free space');
  assert.deepEqual(await mini.rpc('getStorageSpace'), formatted, 'the format answers with the space of the new filesystem');
  assert.deepEqual(await files(mini), [], 'the card has no file after the format');
  const { initialized, cardPresent, filesystemMounted } = await mini.rpc('getStorageState');
  assert.deepEqual({ initialized, cardPresent, filesystemMounted }, { initialized: true, cardPresent: true, filesystemMounted: true },
    'the card is ready after the format');

  await upload(mini, after, seededBytes(2, SIZE));
  assert.deepEqual(await files(mini), [{ filename: after, size: SIZE }], 'the formatted card takes an upload');
  await mountsFromCard(mini, after);
});

test('a format is refused while an image from the card is mounted, and the image stays', { timeout: 180_000 }, async t => {
  const { mini, name } = await bench(t);
  const filename = name('mounted');
  await upload(mini, filename, seededBytes(3, SIZE));
  await mini.rpc('mountWithStorage', { filename, mode: 'Disk' }, 60_000);

  await assert.rejects(mini.rpc('formatStorage', {}, FORMAT_MS), IMAGE_MOUNTED);
  assert.equal((await mini.rpc('getVirtualMediaState'))?.filename, filename, 'the image is still mounted');
  assert.ok((await files(mini)).some(file => file.filename === filename), 'the image is still on the card');
  await mini.rpc('unmountImage');
  await mountsFromCard(mini, filename);
});

// The POST of an upload holds the card's writer lease while its bytes
// arrive. The client sends the first part and waits; the Mini has written
// some of it when the format is asked for.
test('a format is refused while an upload writes to the card, and the next upload works', { timeout: 180_000 }, async t => {
  const { mini, undo, name } = await bench(t);
  const writing = name('writing');
  const next = name('next');
  const total = 16 * SIZE;
  const socket = { current: null };
  undo.push(() => socket.current?.destroy());
  const uploadWrite = async () => (await webrtcStats(mini)).usbHid.uploadWrite;
  const { dataChannel } = await mini.rpc('startStorageFileUpload', { filename: writing, size: total });
  const written = (await uploadWrite()).bytesWritten;
  const url = new URL(`/storage/upload?uploadId=${encodeURIComponent(dataChannel)}`, mini.url);
  socket.current = connectTcp({ host: url.hostname, port: Number(url.port || 80) });
  await new Promise((resolve, reject) => {
    socket.current.once('connect', resolve);
    socket.current.once('error', reject);
  });
  socket.current.on('error', () => {});
  socket.current.resume();
  socket.current.write(`POST ${url.pathname}${url.search} HTTP/1.1\r\nHost: ${url.host}\r\n` +
    `Content-Type: application/octet-stream\r\nContent-Length: ${total}\r\nConnection: close\r\n\r\n`);
  socket.current.write(seededBytes(4, SIZE));
  await until(uploadWrite, now => now.bytesWritten > written, 30);

  await assert.rejects(mini.rpc('formatStorage', {}, FORMAT_MS), UPLOAD_ACTIVE);
  socket.current.destroy();
  // The Mini ends the upload's writer lease when it sees the connection
  // close; until then it refuses the next upload's start.
  await until(() => upload(mini, next, seededBytes(5, SIZE)).then(() => true), Boolean, 30);
  assert.ok((await files(mini)).some(file => file.filename === next && file.size === SIZE), 'an upload after the refusal is whole');
  await mountsFromCard(mini, next);
});
