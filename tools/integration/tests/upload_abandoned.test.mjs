import assert from 'node:assert/strict';
import { test } from 'node:test';
import { seededBytes, sha256 } from '../../combined_load/runner/content.mjs';
import { connect, until } from '../lib/mini.mjs';
import { MAX_FREEZE_MS, hdmiCapture, watchVideo } from '../lib/video.mjs';

const SIZE = 256 * 1024;

// An upload started over RPC whose POST never arrives must not block later
// uploads, including of the same file.
test('an upload whose data never arrives does not block the next upload', { timeout: 180_000 }, async t => {
  const mini = await connect();
  const stamp = String(Date.now());
  const abandoned = { filename: `integration-upload-abandoned-${stamp}.img`, data: seededBytes(1, SIZE) };
  const next = { filename: `integration-upload-next-${stamp}.img`, data: seededBytes(2, SIZE) };
  // The files of this run on the card, by name, with their sizes.
  const files = async () => Object.fromEntries((await mini.rpc('listStorageFiles')).files
    .filter(file => file.filename.includes(stamp)).map(file => [file.filename, file.size]));
  t.after(async () => {
    try {
      // The Mini deletes no file that an upload still writes.
      await until(async () => {
        const left = Object.keys(await files());
        for (const filename of left) await mini.rpc('deleteStorageFile', { filename });
        return left;
      }, left => left.length === 0, 60);
    } finally {
      await mini.close();
    }
  });
  const start = ({ filename, data }) => mini.rpc('startStorageFileUpload', { filename, size: data.length, sha256: sha256(data) });
  // A Wi-Fi Mini holds video off while it takes a POST (the walk judges the
  // video beside an upload), so the watch leaves out each POST, up to the
  // next new frame after it.
  const posts = [];
  const send = async (uploadId, { data }) => {
    const from = Date.now();
    const response = await fetch(new URL(`/storage/upload?uploadId=${encodeURIComponent(uploadId)}`, mini.url), {
      method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: data, signal: AbortSignal.timeout(60_000),
    });
    assert.equal(response.status, 200, await response.text());
    const frames = (await mini.frames()) ?? 0;
    await until(mini.frames, count => count > frames, 30);
    posts.push([from, Date.now()]);
  };
  // HDMI capture has a new frame at a fixed rate, which the waits for the
  // video after a POST need; service video has one only when the host's
  // screen changes. The watch judges a video that runs.
  await hdmiCapture(mini);
  await until(mini.frames, count => count > 0, 30);
  const startedAt = Date.now();
  const video = watchVideo(mini);
  t.after(video);

  const first = await start(abandoned);
  assert.equal(first.alreadyUploadedBytes, 0, 'a new upload starts at 0');

  const second = await start(next);
  await send(second.dataChannel, next);

  const again = await start(abandoned);
  assert.equal(again.alreadyUploadedBytes, 0, 'the card has nothing of the upload that got no data');
  await send(again.dataChannel, abandoned);

  assert.deepEqual(await files(), { [abandoned.filename]: SIZE, [next.filename]: SIZE }, 'the card has both files, whole, and no partial file');

  const end = Date.now();
  let from = startedAt;
  for (const [to, after] of [...posts, [end, end]]) {
    const freeze = video.between(from, to);
    t.diagnostic(`longest time without a video frame ${freeze.ms} ms, outside a POST`);
    assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
    from = after;
  }
  assert.ok(end - startedAt <= 60_000, 'the test takes 60 seconds at most');
});
