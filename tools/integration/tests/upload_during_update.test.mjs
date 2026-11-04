import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';
import { updateServer } from '../lib/servers.mjs';

// esp_reset_reason(): a restart that the firmware asked for, as after an update.
const ESP_RST_SW = 3;
const REFUSAL = /A firmware update is in progress/;

// A write to the TF card fails beside the flash writes of an update, so the
// Mini takes no upload, no download and no format of the card from the
// request for an update until its restart. The update itself goes on.
test('an upload start and a format of the card are refused while an update is in progress', { timeout: 300_000 }, async t => {
  const mini = await connect();
  let server;
  // Names of this run alone: the cleanup removes no file that was on the
  // card before.
  const upload = `integration-upload-${Date.now()}.img`;
  const download = `integration-download-${Date.now()}.iso`;
  let startedAt = null;
  t.after(async () => {
    try {
      // An update that this test started ends before the bench is given back.
      if (startedAt !== null) {
        await until(async () => (await mini.restarted(startedAt)) || (await mini.rpc('isUpdatePending')) === false, Boolean, 180)
          .catch(() => undefined);
      }
      // What a Mini that did not refuse has begun on its card.
      await mini.open();
      for (const filename of [upload, `${upload}.incomplete`, download, `${download}.incomplete`]) {
        await mini.rpc('deleteStorageFile', { filename }).catch(() => undefined);
      }
    } finally {
      // Also when the Mini does not come back: an open browser or server
      // keeps the test's process alive.
      await mini.close().finally(() => server?.close());
    }
  });

  server = await updateServer(mini);
  await mini.rpc('setUpdateApiUrl', { url: server.base });
  startedAt = Date.now();
  // The Mini can restart before its answer arrives.
  mini.rpc('tryUpdate', {}, 30_000).catch(() => undefined);
  // In progress: the Mini reports that it has taken a part of the image,
  // which takes tens of seconds in all. isUpdatePending alone can be false
  // for an instant between the request and the update's start.
  await until(() => mini.events('otaState'), states => states.some(state => state.systemDownloadProgress > 0), 30);
  assert.equal(await mini.rpc('isUpdatePending'), true);

  await assert.rejects(mini.rpc('startStorageFileUpload', { filename: upload, size: 1024 * 1024 }), REFUSAL);
  await assert.rejects(mini.rpc('downloadToStorage', { url: `${server.base}/combined-load.iso`, filename: download }), REFUSAL);
  await assert.rejects(mini.rpc('formatStorage'), /A firmware update is in progress\. Format the card after the update has finished\./);

  await until(() => mini.restarted(startedAt), restarted => restarted, 180);
  const after = await mini.status();
  assert.equal(after.firmware.elfSha256, mini.image.elfSha256, 'the Mini runs the image after the update');
  assert.equal(after.resetReason, ESP_RST_SW, 'the restart was the firmware\'s');
});
