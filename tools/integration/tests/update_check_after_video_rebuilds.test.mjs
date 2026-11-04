import assert from 'node:assert/strict';
import net from 'node:net';
import { test } from 'node:test';
import { addressTowards } from '../../combined_load/runner/switches.mjs';
import { hostDisplay } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { rpcEvery100ms } from '../lib/session.mjs';
import { decodes, hdmiCapture, webrtcStats } from '../lib/video.mjs';

// The largest free block depends on the video mode, so every sample is taken
// in one mode: each round trip sets the host to the other mode and back,
// which builds the capture again twice. 1080p30 and 720p60 differ in the
// size of the encoder and of the capture's buffers.
const MODE = { width: 1280, height: 720, hz: 60 };
const OTHER = { width: 1920, height: 1080, hz: 30 };
const ROUND_TRIPS = 10;
// One rebuild can place its buffers so that the largest block is up to 1 MiB
// smaller than after the rebuild before, and a later rebuild gives it back.
// Fragmentation that adds up lowers every later sample, so the best of the
// first three samples is compared with the best of the last three.
const WINDOW = 3;
const TOLERANCE = 1024 * 1024;
// What an update check's TLS connection leaves of internal RAM and of
// DMA-capable RAM above the Mini's floors: over twice the 6.6 kB that one
// such connection takes.
const TLS_HEADROOM = 16 * 1024;
const PRODUCTION = 'https://api.jetkvm.com';

// Linux host. Capture rebuilds on mode changes must not fragment PSRAM or
// starve TLS admission (HTTPS image check, update check).
test('after ten round trips of the host\'s resolution the largest free PSRAM block is as large, HTTPS image checks are admitted and an update check has its TLS headroom',
  { timeout: (2 * ROUND_TRIPS + 6) * 60_000 }, async t => {
    const mini = await connect();
    let display = null;
    let traffic = null;
    // The Mini reserves the TLS memory of an image check before it connects,
    // so the image's address is a port of this machine that closes every
    // connection, and the check's own result does not count.
    const closing = net.createServer(socket => socket.destroy());
    t.after(async () => {
      try {
        await traffic?.stop();
        closing.close();
        await display?.restore();
      } finally {
        await mini.close();
      }
    });
    // First, so that the mode before the test is the one of the Mini's own
    // display and not of a sender's service video.
    await hdmiCapture(mini);
    display = await hostDisplay(t, mini);
    if (!display) return;

    const stats = () => webrtcStats(mini, 'details=1');
    const admissions = async owner => (await stats()).memoryBudget.tlsAdmissions.find(entry => entry.owner === owner) ??
      { admitted: 0, denied: 0 };
    // Sets the host's mode and waits until the Mini captures it and the page
    // decodes video again. The Mini's video epoch advances when the old mode
    // is cleared and again when the new one is published.
    const capture = async mode => {
      const before = await stats();
      const had = await display.set(mode);
      const changed = had.width !== mode.width || had.height !== mode.height || Math.round(had.hz) !== mode.hz;
      const now = await until(stats, found => found.modeValid && found.width === mode.width && found.height === mode.height &&
        found.fps === mode.hz && (!changed || found.videoEpoch >= before.videoEpoch + 2), 45);
      await decodes(mini, 30);
      return now;
    };

    const start = await stats();
    assert.equal(start.modeValid, true, 'the Mini captures the host\'s HDMI video');
    const address = await addressTowards(new URL(mini.url).hostname);
    await new Promise(resolve => closing.listen(0, address, resolve));
    const image = `https://${address}:${closing.address().port}/memory.iso`;

    // RPC traffic beside the rebuilds, as a page that is open sends it.
    traffic = rpcEvery100ms(mini);
    await capture(MODE);
    const largest = [];
    for (let trip = 1; trip <= ROUND_TRIPS; trip++) {
      await capture(OTHER);
      const back = await capture(MODE);
      assert.equal(back.memoryBudget.videoRebuildFailures, start.memoryBudget.videoRebuildFailures, `no rebuild failed (round trip ${trip})`);
      largest.push(back.psramLargest);
      const before = await admissions('https-media-check');
      await mini.rpc('checkMountUrl', { url: image }, 30_000).catch(() => undefined);
      const after = await admissions('https-media-check');
      const line = `round trip ${trip}: PSRAM free ${back.psramFree}, largest block ${back.psramLargest}, ` +
        `image check admitted +${after.admitted - before.admitted}, refused +${after.denied - before.denied}`;
      t.diagnostic(line);
      assert.equal(after.denied - before.denied, 0, line);
      assert.equal(after.admitted - before.admitted, 1, line);
    }
    const { sent, failures } = await traffic.stop();
    traffic = null;
    t.diagnostic(`${sent} RPC calls beside the rebuilds, ${failures.length} failed`);
    assert.deepEqual(failures, [], `RPC calls beside the rebuilds that failed, of ${sent}`);

    const early = Math.max(...largest.slice(0, WINDOW));
    const late = Math.max(...largest.slice(-WINDOW));
    t.diagnostic(`largest free PSRAM block: best of the first ${WINDOW} round trips ${early}, of the last ${WINDOW} ${late}`);
    assert.ok(late >= early - TOLERANCE, `the largest free PSRAM block after each round trip: ${largest.join(', ')}`);

    // "Check for updates" against the real update server, from a page that
    // shows video.
    await mini.rpc('setUpdateApiUrl', { url: PRODUCTION });
    const before = await admissions('ota-download');
    const answer = await mini.rpc('getUpdateStatus', {}, 30_000);
    const after = await admissions('ota-download');
    t.diagnostic(`update check: internal RAM ${after.lastInternalHeadroom} B and DMA-capable RAM ${after.lastDmaHeadroom} B above the floors`);
    assert.equal(after.denied, before.denied, `the Mini refused the update check: ${JSON.stringify(after)}`);
    assert.ok(after.admitted > before.admitted, 'the update check took the TLS reservation of a firmware download');
    assert.ok(answer.remote?.systemVersion, 'the update server offers a release');
    assert.ok(after.lastInternalHeadroom >= TLS_HEADROOM, `internal RAM above the floor: ${after.lastInternalHeadroom}`);
    assert.ok(after.lastDmaHeadroom >= TLS_HEADROOM, `DMA-capable RAM above the floor: ${after.lastDmaHeadroom}`);
  });
