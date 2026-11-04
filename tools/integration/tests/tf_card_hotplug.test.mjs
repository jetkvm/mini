import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';
import { seededBytes } from '../../combined_load/runner/content.mjs';
import { addressTowards } from '../../combined_load/runner/switches.mjs';
import { connectHost } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';
import { webrtcStats } from '../lib/video.mjs';

// A bench build takes the TF card out with the call testTfCardOut: the card
// reads as removed on its card-detect pin and loses its supply, as a card
// that a hand pulls from the slot. {out: false} puts it back. The card stays
// in the slot, so a physical removal and insertion are tested by hand.

const MIB = 1024 * 1024;
// The Mini reads the card-detect pin every 250 ms and acts on two equal
// readings; setting up an inserted card powers it through a reset of
// 600 ms and mounts it.
const REMOVED_WITHIN_S = 5;
const READY_WITHIN_S = 15;

const storage = mini => mini.rpc('getStorageState');
const ready = state => state.initialized && state.cardPresent && state.filesystemMounted;

// The Mini with its card in at the start and at the end of a test. The
// cleanup runs what the test put into `undo`, puts the card back, waits until
// it is mounted, deletes the test's files and closes the page; a card that
// does not come back fails the test.
async function withCard(t, files = []) {
  const mini = await connect();
  const undo = [];
  t.after(async () => {
    try {
      for (const step of undo.reverse()) await step();
      await mini.rpc('testTfCardOut', { out: false });
      await until(() => storage(mini), ready, READY_WITHIN_S)
        .catch(e => assert.fail(`the card is mounted again after the test within ${READY_WITHIN_S} seconds: ${e.message}`));
      for (const filename of files.flatMap(name => [name, `${name}.incomplete`])) {
        await mini.rpc('deleteStorageFile', { filename }).catch(e => {
          if (!/file does not exist/.test(e.message)) throw e;
        });
      }
    } finally {
      await mini.close();
    }
  });
  const state = await storage(mini);
  assert.ok(ready(state), `the bench's Mini has a mounted TF card before the test: ${JSON.stringify(state)}`);
  return { mini, generation: state.cardGeneration, undo };
}

async function takeOut(mini, generation) {
  await mini.rpc('testTfCardOut', { out: true });
  const state = await until(() => storage(mini), found => !found.cardPresent && !found.filesystemMounted, REMOVED_WITHIN_S)
    .catch(e => assert.fail(`the Mini reports no card within ${REMOVED_WITHIN_S} seconds: ${e.message}`));
  assert.equal(state.cardGeneration, generation + 1, 'the card generation advances once with the removal');
  return state;
}

async function putBack(mini) {
  await mini.rpc('testTfCardOut', { out: false });
  return until(() => storage(mini), ready, READY_WITHIN_S)
    .catch(e => assert.fail(`the Mini mounts the card within ${READY_WITHIN_S} seconds: ${e.message}`));
}

// One file on the TF card, as the page uploads it: the start call and one POST.
async function upload(mini, filename, data) {
  const { dataChannel } = await mini.rpc('startStorageFileUpload', { filename, size: data.length });
  const response = await fetch(new URL(`/storage/upload?uploadId=${encodeURIComponent(dataChannel)}`, mini.url), {
    method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: data, signal: AbortSignal.timeout(120_000),
  });
  assert.equal(response.status, 200, `the upload of ${filename}: ${await response.text()}`);
}

const names = async mini => (await mini.rpc('listStorageFiles')).files.map(file => file.filename).sort();

test('a card that is taken out and put back is mounted again without a restart', { timeout: 120_000 }, async t => {
  const startedAt = Date.now();
  const { mini, generation } = await withCard(t);
  const before = await names(mini);

  await takeOut(mini, generation);
  await assert.rejects(mini.rpc('listStorageFiles'), 'the Mini lists no files without a card');
  await assert.rejects(mini.rpc('startStorageFileUpload', { filename: 'integration-no-card.img', size: MIB }),
    'the Mini starts no upload without a card');

  const state = await putBack(mini);
  assert.equal(state.cardGeneration, generation + 1, 'the insertion keeps the generation of the removal');
  assert.deepEqual(await names(mini), before, 'the Mini lists the same files on the card');
  assert.equal(await mini.restarted(startedAt), false, 'the Mini did not restart');
});

test('taking the card out ends the image mounted from it, and the host has no medium', { timeout: 300_000 }, async t => {
  const filename = `integration-hotplug-${Date.now()}.iso`;
  const { mini, generation, undo } = await withCard(t, [filename]);
  undo.push(await hostTestUsb(mini));
  await upload(mini, filename, seededBytes(1, 4 * MIB));
  await mini.rpc('mountWithStorage', { filename, mode: 'CDROM' }, 60_000);
  assert.equal((await mini.rpc('getVirtualMediaState'))?.filename, filename, 'the image is mounted');

  // A Linux host reports the medium in the Mini's CD-ROM drive.
  const host = await connectHost(mini);
  const linux = host.system === 'linux';
  const medium = () => host.exec(
    'for d in $(lsblk -Snpo NAME,TRAN,TYPE,VENDOR 2>/dev/null | awk \'$2=="usb" && $3=="rom" && $4=="JetKVM" {print $1}\'); do ' +
    'sudo -n blockdev --getsize64 "$d" 2>/dev/null || echo 0; done; true').then(out => out.trim());
  if (linux) {
    await until(medium, size => size === String(4 * MIB), 45)
      .catch(e => assert.fail(`the host has the image in the Mini's drive within 45 seconds: ${e.message}`));
  } else {
    t.diagnostic(`the host is ${host.system}: the test reads the medium on a Linux host only`);
  }

  await takeOut(mini, generation);
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'nothing is mounted after the removal');
  if (linux) {
    await until(medium, size => size === '0' || size === '', 15)
      .catch(e => assert.fail(`the host's drive has no medium within 15 seconds: ${e.message}`));
  }

  await putBack(mini);
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'the insertion mounts no image');
  await mini.rpc('mountWithStorage', { filename, mode: 'CDROM' }, 60_000);
  assert.equal((await mini.rpc('getVirtualMediaState'))?.filename, filename, 'the image mounts from the card again');
  await mini.rpc('unmountImage', {}, 60_000);
});

test('taking the card out ends an upload to it with an error, and the next upload works', { timeout: 300_000 }, async t => {
  const stamp = Date.now();
  const interrupted = `integration-hotplug-upload-${stamp}.img`;
  const next = `integration-hotplug-next-${stamp}.img`;
  const { mini, generation } = await withCard(t, [interrupted, next]);
  const SIZE = 1024 * MIB;
  const CHUNK = MIB;
  const TAKE_OUT_AT = 16 * MIB;

  const { dataChannel } = await mini.rpc('startStorageFileUpload', { filename: interrupted, size: SIZE });
  let sent = 0;
  let removal = null;
  // The page's POST has a Content-Length, which the Mini needs.
  const outcome = await new Promise(resolve => {
    const request = http.request(new URL(`/storage/upload?uploadId=${encodeURIComponent(dataChannel)}`, mini.url), {
      method: 'POST', headers: { 'Content-Type': 'application/octet-stream', 'Content-Length': SIZE }, timeout: 120_000,
    }, response => {
      response.resume();
      resolve(`HTTP ${response.statusCode}`);
      request.destroy();
    });
    request.on('error', error => resolve(`${error.code ?? error.message}`));
    request.on('timeout', () => request.destroy(new Error('no end within 120 seconds')));
    const write = () => {
      while (sent < SIZE && !request.destroyed) {
        const more = request.write(seededBytes(sent / CHUNK, CHUNK));
        sent += CHUNK;
        if (sent >= TAKE_OUT_AT && !removal) removal = takeOut(mini, generation);
        if (!more) return request.once('drain', write);
      }
      if (sent >= SIZE) request.end();
    };
    write();
  });
  t.diagnostic(`the upload ended with ${outcome} after ${sent / MIB} MiB were sent`);
  assert.ok(removal, `the test took the card out after ${TAKE_OUT_AT / MIB} MiB`);
  await removal;
  assert.notEqual(outcome, 'HTTP 200', 'the upload does not succeed');
  assert.ok(sent < SIZE, 'the Mini ends the upload before the whole body is sent');
  // A Wi-Fi Mini holds video off during an upload and gives it back.
  await until(() => webrtcStats(mini), stats => !(stats.videoPausedBy ?? []).includes('upload'), 10)
    .catch(e => assert.fail(`the upload holds video off no more within 10 seconds: ${e.message}`));

  await putBack(mini);
  await upload(mini, next, seededBytes(7, 2 * MIB));
  assert.ok((await names(mini)).includes(next), 'the next upload is on the card');
});

test('taking the card out ends a download to it with an error the page shows', { timeout: 300_000 }, async t => {
  const filename = `integration-hotplug-download-${Date.now()}.iso`;
  const { mini, generation } = await withCard(t, [filename]);
  const SIZE = 1024 * MIB;
  const CHUNK = MIB;
  // A server on this machine that sends the body at most 8 MiB/s, so the
  // download runs for minutes.
  const address = await addressTowards(new URL(mini.url).hostname);
  const server = http.createServer((request, response) => {
    response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': SIZE });
    let offset = 0;
    const send = () => {
      if (offset >= SIZE || response.destroyed) return response.end();
      response.write(seededBytes(offset / CHUNK, CHUNK));
      offset += CHUNK;
      setTimeout(send, 125);
    };
    send();
  });
  await new Promise(resolve => server.listen(0, address, resolve));
  t.after(() => {
    server.closeAllConnections();
    server.close();
  });

  await mini.rpc('downloadToStorage', { url: `http://${address}:${server.address().port}/${filename}`, filename }, 30_000);
  await until(() => mini.rpc('getStorageDownloadState'), state => state.bytes >= 8 * MIB, 60)
    .catch(e => assert.fail(`the download writes 8 MiB within 60 seconds: ${e.message}`));
  await takeOut(mini, generation);
  const ended = await until(() => mini.rpc('getStorageDownloadState'), state => state.state !== 'downloading', 30)
    .catch(e => assert.fail(`the download ends within 30 seconds of the removal: ${e.message}`));
  assert.equal(ended.state, 'error');
  assert.equal(ended.error, 'The TF card was removed during the download.');
  const event = (await until(() => mini.events('storageDownloadState'), events => events.at(-1)?.state === 'error', 5)
    .catch(e => assert.fail(`the page gets the error as a storageDownloadState event: ${e.message}`))).at(-1);
  assert.equal(event.error, ended.error);
  assert.equal(event.videoPaused, false, 'the download holds video off no more');
});
