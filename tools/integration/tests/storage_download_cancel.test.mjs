import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';
import { seededBytes } from '../../combined_load/runner/content.mjs';
import { addressTowards } from '../../combined_load/runner/switches.mjs';
import { connect, until } from '../lib/mini.mjs';

// cancelStorageDownload stops a download to the TF card, deletes its partial
// file and frees the card, also while an image from the card is mounted.

const MIB = 1024 * 1024;
const CANCELLED_WITHIN_S = 20;

const names = async mini => (await mini.rpc('listStorageFiles')).files.map(file => file.filename);

// A server on this machine. The large file comes at most 8 MiB/s, so its
// download runs for minutes; the small one comes at once.
async function imageServer(t, mini, size) {
  const CHUNK = MIB;
  const address = await addressTowards(new URL(mini.url).hostname);
  const server = http.createServer((request, response) => {
    const length = request.url.includes('small') ? 2 * MIB : size;
    response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': length });
    let offset = 0;
    const send = () => {
      if (offset >= length || response.destroyed) return response.end();
      response.write(seededBytes(offset / CHUNK, CHUNK));
      offset += CHUNK;
      setTimeout(send, length === size ? 125 : 0);
    };
    send();
  });
  await new Promise(resolve => server.listen(0, address, resolve));
  t.after(() => {
    server.closeAllConnections();
    server.close();
  });
  return name => `http://${address}:${server.address().port}/${name}`;
}

async function downloaded(mini, url, filename) {
  await mini.rpc('startStorageDownload', { url, filename }, 30_000);
  const ended = await until(() => mini.rpc('getStorageDownloadState'), state => state.status !== 'downloading', 60)
    .catch(e => assert.fail(`the download of ${filename} ends within 60 seconds: ${e.message}`));
  assert.equal(ended.status, 'done', `the download of ${filename} ends as done: ${JSON.stringify(ended)}`);
}

test('a cancelled download leaves no file, and the card takes the next download and mount', { timeout: 300_000 }, async t => {
  const stamp = Date.now();
  const large = `integration-cancel-${stamp}.iso`;
  const small = `integration-cancel-small-${stamp}.iso`;
  const mini = await connect();
  t.after(async () => {
    try {
      await mini.rpc('cancelStorageDownload').catch(() => undefined);
      await mini.rpc('unmountImage', {}, 60_000).catch(() => undefined);
      for (const filename of [large, `${large}.incomplete`, small]) {
        await mini.rpc('deleteStorageFile', { filename }).catch(e => {
          if (!/file does not exist/.test(e.message)) throw e;
        });
      }
    } finally {
      await mini.close();
    }
  });
  const capabilities = await mini.rpc('getDeviceCapabilities');
  assert.ok(capabilities.includes('storage_download'), 'the Mini reports storage_download');
  const card = await mini.rpc('getStorageState');
  assert.ok(card.cardPresent && card.filesystemMounted, `the bench's Mini has a mounted TF card: ${JSON.stringify(card)}`);
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'nothing is mounted before the test');
  const url = await imageServer(t, mini, 1024 * MIB);

  const cancel = async what => {
    const started = await mini.rpc('startStorageDownload', { url: url(large), filename: large }, 30_000);
    assert.equal(started.status, 'downloading', 'startStorageDownload answers the state');
    assert.equal(started.filename, large);
    await until(() => mini.rpc('getStorageDownloadState'), state => state.received >= 8 * MIB, 60)
      .catch(e => assert.fail(`${what}: the download writes 8 MiB within 60 seconds: ${e.message}`));
    assert.equal(await mini.rpc('cancelStorageDownload'), null);
    const ended = await until(() => mini.rpc('getStorageDownloadState'), state => state.status !== 'downloading', CANCELLED_WITHIN_S)
      .catch(e => assert.fail(`${what}: the download ends within ${CANCELLED_WITHIN_S} seconds of the cancel: ${e.message}`));
    assert.equal(ended.status, 'cancelled', `${what}: ${JSON.stringify(ended)}`);
    assert.equal(ended.state, 'cancelled', 'the Mini\'s own state field agrees');
    assert.equal(ended.error, '', `${what}: the partial file is deleted`);
    assert.ok(ended.received < ended.size, `${what}: the download stopped before the end`);
    const event = (await until(() => mini.events('storageDownloadState'), events => events.at(-1)?.status === 'cancelled', 5)
      .catch(e => assert.fail(`${what}: the page gets the cancel as a storageDownloadState event: ${e.message}`))).at(-1);
    assert.equal(event.filename, large);
    assert.equal(event.videoPaused, false, `${what}: the download holds video off no more`);
    const files = await names(mini);
    assert.ok(!files.includes(large) && !files.includes(`${large}.incomplete`),
      `${what}: no file of the cancelled download is on the card: ${files.join(', ')}`);
  };

  await cancel('nothing mounted');
  // The card is free: the next download writes, and its file mounts.
  await downloaded(mini, url(small), small);
  await mini.rpc('mountWithStorage', { filename: small, mode: 'CDROM' }, 60_000);
  assert.equal((await mini.rpc('getVirtualMediaState'))?.filename, small);

  // A mounted image does not keep the partial file of a cancelled download.
  await cancel('an image mounted');
  assert.equal((await mini.rpc('getVirtualMediaState'))?.filename, small, 'the image stays mounted');
  await mini.rpc('unmountImage', {}, 60_000);
  assert.equal(await mini.rpc('cancelStorageDownload'), null, 'a cancel without a download answers null');
});
