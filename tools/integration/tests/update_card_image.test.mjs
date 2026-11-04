import assert from 'node:assert/strict';
import { test } from 'node:test';
import { imageBytes } from '../../combined_load/runner/content.mjs';
import { connectHost, hostReadsImage } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { updateServer } from '../lib/servers.mjs';
import { hostTestUsb } from '../lib/usb.mjs';

// esp_reset_reason(): a restart that the firmware asked for, as after an update.
const ESP_RST_SW = 3;
const FILE_BLOCKS = 16;

// One file on the TF card, as the page uploads it: the start call and one POST.
async function upload(mini, filename, data) {
  const { dataChannel } = await mini.rpc('startStorageFileUpload', { filename, size: data.length });
  const response = await fetch(new URL(`/storage/upload?uploadId=${encodeURIComponent(dataChannel)}`, mini.url), {
    method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: data, signal: AbortSignal.timeout(60_000),
  });
  assert.equal(response.status, 200, `the upload of ${filename}: ${await response.text()}`);
}

// readErrors is only reported: Linux sends unsupported commands to new disks.
async function hostReadsFile(t, mini, host, name, data, when) {
  const { blocksWrong, readErrors } = await hostReadsImage(mini, host, name, data);
  t.diagnostic(`${when}: ${readErrors} commands answered with an error`);
  assert.equal(blocksWrong, 0, `the host reads the file as it was uploaded, ${when}`);
}

// A user mounts an image from the TF card, installs an update with "Update
// in Background", and mounts the image again after the restart. The update
// writes the other slot of the Mini's flash and restarts the Mini, which
// ends the mount; the card and its file stay as they were.
test('an image on the TF card mounts and reads the same after an update that ran beside its mount', { timeout: 600_000 }, async t => {
  const mini = await connect();
  const host = await connectHost(mini).catch(async error => {
    await mini.close();
    throw error;
  });
  if (host.commands.releaseMedia) {
    await mini.close();
    t.skip(`the test reads the card through a disk on the host, and ${host.login} is a ${host.system} host, which keeps a disk that the Mini unmounts`);
    return;
  }
  // A name of this run alone: the cleanup removes no file that was on the card before.
  const name = `integration-update-image-${Date.now()}.img`;
  let server;
  let startedAt = null;
  let restoreUsb = null;
  t.after(async () => {
    try {
      // An update that this test started ends before the bench is given back.
      if (startedAt !== null) {
        await until(async () => (await mini.restarted(startedAt)) || (await mini.rpc('isUpdatePending')) === false, Boolean, 180)
          .catch(() => undefined);
      }
      try {
        // The USB state and the mounted image as found; this also unmounts the test's file.
        await restoreUsb?.();
      } finally {
        for (const filename of [name, `${name}.incomplete`]) {
          await mini.rpc('deleteStorageFile', { filename }).catch(e => {
            if (!/file does not exist/.test(e.message)) throw e;
          });
        }
      }
    } finally {
      // Also when the Mini does not come back: an open browser or server
      // keeps the test's process alive.
      await mini.close().finally(() => server?.close());
    }
  });
  // The host tests' USB devices, with the drive and nothing mounted.
  restoreUsb = await hostTestUsb(mini);

  const data = imageBytes(Math.floor(Date.now() / 1000), FILE_BLOCKS);
  await upload(mini, name, data);
  await hostReadsFile(t, mini, host, name, data, 'before the update');

  server = await updateServer(mini);
  await mini.rpc('setUpdateApiUrl', { url: server.base });
  await mini.rpc('mountWithStorage', { filename: name, mode: 'Disk' }, 60_000);
  startedAt = Date.now();
  // The Mini can restart before its answer arrives.
  mini.rpc('tryUpdate', {}, 30_000).catch(() => undefined);
  await until(() => mini.restarted(startedAt), restarted => restarted, 180);
  const after = await mini.status();
  assert.equal(server.served.firmwareBytes, mini.image.bytes.length, 'the Mini took the whole image');
  assert.equal(after.firmware.elfSha256, mini.image.elfSha256, 'the Mini runs the image after the update');
  assert.equal(after.resetReason, ESP_RST_SW, 'the restart was the firmware\'s');

  await mini.open();
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'the restart of the update ended the mount');
  await hostReadsFile(t, mini, host, name, data, 'after the update');
});
