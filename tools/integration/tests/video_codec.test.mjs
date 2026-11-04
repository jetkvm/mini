import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';
import { MAX_FREEZE_MS, watchVideo } from '../lib/video.mjs';

// The preference is for the next session: the video of this one goes on.
// connect() records the preference, and close() stores it again.
test('the codec preference is stored for each supported codec and for auto, and vp9 is refused', { timeout: 90_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  // The watch judges a video that runs.
  await until(mini.frames, count => count > 0, 30);
  const video = watchVideo(mini);
  t.after(video);

  const supported = await mini.rpc('getSupportedVideoCodecs');
  assert.ok(supported.includes('h264'), `the Mini encodes h264: ${JSON.stringify(supported)}`);
  for (const codec of [...supported, 'auto']) {
    await mini.rpc('setVideoCodecPreference', { codec });
    assert.equal(await mini.rpc('getVideoCodecPreference'), codec);
  }
  await assert.rejects(mini.rpc('setVideoCodecPreference', { codec: 'vp9' }));
  assert.equal(await mini.rpc('getVideoCodecPreference'), 'auto', 'the refused codec is not stored');

  const freeze = video();
  t.diagnostic(`longest time without a video frame ${freeze.ms} ms`);
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
});
