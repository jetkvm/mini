import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { after, before, describe, test } from 'node:test';
import { closeDiskDialogs } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hook, openSession, videoState } from '../lib/os_services.mjs';
import { senderHost } from '../lib/sender_host.mjs';
import { updateServer } from '../lib/servers.mjs';

const WIDTH = 1920;
const HEIGHT = 1080;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// A mount that changes the drive type re-enumerates USB, ending the OS
// Services session; a CD-ROM image on a CD-ROM drive does not.
describe('OS Services service video and a mount', () => {
  let mini;
  let host;
  let session;
  let server;
  let watch;
  let skip;
  // A Mini that the tests are not for skips each of them.
  const step = (name, options, run) => test(name, options, t => (skip ? t.skip(skip) : run(t)));

  // One answer of the Mini's /webrtc/stats, with 10 seconds for it. A failure names the request and its time.
  const stats = async query => {
    const asked = Date.now();
    try {
      return await (await fetch(new URL(`/webrtc/stats?${query}`, mini.url), { signal: AbortSignal.timeout(10_000) })).json();
    } catch (error) {
      throw new Error(`/webrtc/stats?${query} of the Mini: ${error.name}: ${error.message}${error.cause ? ` (${error.cause.message})` : ''}, ${Date.now() - asked} ms after the request`);
    }
  };
  // The Mini's counts for service video: source, switches and more.
  const serviceVideo = async () => (await stats('bitrate=1')).guestVideo;
  // The Mini's side of the host's USB. mountCount counts the times that the
  // host has configured the Mini, so it grows when the Mini enumerates
  // again; commands counts the host's commands to the drive that are no read.
  const hostUsb = async () => {
    const usb = (await stats('details=1')).usbHid;
    return { configured: usb.mounted === true, mountCount: usb.mountCount, commands: usb.mscRead.nonReadCommands };
  };
  // Service video in the settings' size, in the Mini's state and in the browser.
  const showsServiceVideo = async () => {
    const state = await until(() => videoState(mini), now => now.osServices.videoError !== undefined ||
      (now.source === 'guest' && now.osServices.video && now.ready && now.width === WIDTH && now.height === HEIGHT), 60);
    assert.equal(state.osServices.videoError, undefined, 'the Mini\'s service video error');
    await until(() => hook(mini, 'getVideoStreamDimensions'), size => size?.width === WIDTH && size?.height === HEIGHT, 30);
  };
  const videoOn = async () => {
    await mini.rpc('setOsServicesSettings', { settings: { video_enabled: true, resolution: `${WIDTH}x${HEIGHT}` } });
    await showsServiceVideo();
  };
  // The walk's watch of the Mini's media on the host: the medium of its
  // newest line, null when the host has none, and undefined while that line
  // is older than 3 seconds.
  let newest;
  let newestAt = 0;
  const medium = () => (Date.now() - newestAt < 3000 ? newest : undefined);
  // The image as the CD-ROM, when the host has it.
  const mountCdrom = async () => {
    await mini.rpc('unmountImage').catch(() => undefined);
    await mini.rpc('mountWithHTTP', { url: server.url, mode: 'CDROM' }, 60_000);
    return until(medium, now => now?.type === 'rom', 45);
  };

  before(async () => {
    mini = await connect();
    try {
      host = await senderHost(mini);
      if ((await mini.rpc('getUsbDevices')).mass_storage !== true) {
        skip = 'the Mini\'s USB devices have no drive (getUsbDevices)';
        return;
      }
      assert.equal(await mini.rpc('getVirtualMediaState'), null, 'the tests need a Mini with nothing mounted');
      server = await updateServer(mini);
      watch = host.stream(host.commands.watchMedia, line => {
        const fields = line.trim().split(/\s+/);
        newest = fields[0] === 'none' ? null : host.commands.medium(fields);
        newestAt = Date.now();
      });
      session = await openSession(mini, host);
      // Every switch to service video counts against the Mini's limit of
      // switches, which video off and on clears.
      if ((await mini.rpc('getOsServicesSettings')).video_enabled) {
        await mini.rpc('setOsServicesSettings', { settings: { video_enabled: false } });
        await mini.rpc('setOsServicesSettings', { settings: { video_enabled: true } });
      }
      // A sender registers video some seconds after its session starts.
      const state = await until(() => videoState(mini), now => now.osServices.videoRegistered || now.osServices.videoError !== undefined, 15)
        .catch(() => videoState(mini));
      assert.equal(state.osServices.videoError, undefined, 'the Mini\'s service video error');
      assert.equal(state.osServices.videoRegistered, true, 'the sender offers service video');
      await mini.rpc('setOsServicesSettings', { settings: { video_enabled: false, resolution: `${WIDTH}x${HEIGHT}` } });
      await until(() => videoState(mini), now => now.source === 'physical', 60);
    } catch (error) {
      await cleanup().catch(() => undefined);
      throw error;
    }
  }, { timeout: 300_000 });

  const cleanup = async () => {
    try {
      await watch?.stop();
      server?.close();
      await mini.rpc('unmountImage', {}, 30_000).catch(() => undefined);
      await closeDiskDialogs(mini);
      await session?.restore();
    } finally {
      await mini.close();
    }
  };
  after(() => cleanup(), { timeout: 300_000 });

  // Four mounts are more than the Mini's limit of three switches to service
  // video in two minutes, which is for switches that fail and not for the
  // Mini's own returns to USB.
  step('mounts that change the type of the drive take the Mini off USB and back, and service video returns after each', { timeout: 600_000 }, async () => {
    await videoOn();
    try {
      for (const mode of ['Disk', 'CDROM', 'Disk', 'CDROM']) {
        const { switches } = await serviceVideo();
        const { mountCount } = await hostUsb();
        if (mode === 'CDROM') {
          await mountCdrom();
        } else {
          await mini.rpc('unmountImage');
          await mini.rpc('mountWithHTTP', { url: server.url, mode }, 60_000);
        }
        await until(hostUsb, now => now.mountCount > mountCount, 60);
        const back = await until(async () => ({ error: (await videoState(mini)).osServices.videoError, switches: (await serviceVideo()).switches }),
          now => now.error !== undefined || now.switches > switches, 60);
        assert.equal(back.error, undefined, `the Mini's service video error after the mount as ${mode}`);
        await showsServiceVideo();
      }
    } finally {
      await mini.rpc('unmountImage', {}, 30_000).catch(() => undefined);
    }
  });

  // A Linux host asks its CD-ROM drive for a medium every 2 seconds (the
  // block layer's event poll), with and without a medium, and the Mini keeps
  // USB for a CD-ROM image when the host has asked since the unmount.
  step('a CD-ROM image for a CD-ROM drive that the host asks for its medium keeps USB and service video', { timeout: 300_000 }, async t => {
    if (host.system !== 'linux') return t.skip(`the test reads the CD-ROM as a Linux host does, and ${host.login} runs ${host.system}`);
    // GNU dd reads past the host's cache (iflag=direct); the dd of uutils does not.
    const dd = (await host.exec('sudo -n true 2>/dev/null && for dd in gnudd dd; do ' +
      '"$dd" --version 2>/dev/null | head -n 1 | grep -v uutils | grep -q coreutils && { echo "$dd"; break; }; done; true')).trim();
    if (!dd) return t.skip(`${host.login} needs sudo without a password and GNU dd to read the CD-ROM`);
    // The drive is a CD-ROM drive once a CD-ROM image has been mounted.
    await mountCdrom();
    await videoOn();
    await mini.rpc('unmountImage');
    const emptied = (await hostUsb()).commands;
    const asked = await until(hostUsb, now => now.commands > emptied, 10).catch(() => null);
    if (!asked) return t.skip(`${host.login} did not ask its CD-ROM drive for a medium in 10 s (/sys/module/block/parameters/events_dfl_poll_msecs)`);

    const before = { usb: await hostUsb(), switches: (await serviceVideo()).switches, device: (await host.usb(mini)).device.devnum };
    // The pointer moves on the host's screen, so the sender has a new frame
    // all the time; a session that ended would send none.
    let moving = true;
    const moved = (async () => {
      for (let angle = 0; moving; angle += 0.12) {
        await mini.rpc('absMouseReport', { x: Math.round(16384 + 9000 * Math.cos(angle)), y: Math.round(16384 + 9000 * Math.sin(angle)), buttons: 0 })
          .catch(() => undefined);
        await sleep(16);
      }
    })();
    try {
      const frames = await mini.frames();
      await mini.rpc('mountWithHTTP', { url: server.url, mode: 'CDROM' }, 60_000);
      // For 5 seconds the host keeps the Mini configured as it was, and the
      // source stays service video.
      for (const end = Date.now() + 5000; Date.now() < end; await sleep(250)) {
        const usb = await hostUsb();
        assert.equal(usb.configured, true, 'the host keeps the Mini configured');
        assert.equal(usb.mountCount, before.usb.mountCount, 'the host does not configure the Mini again');
        assert.equal((await serviceVideo()).source, 'guest', 'the video source during the mount');
      }
      assert.equal((await serviceVideo()).switches, before.switches, 'switches of the video source');
      assert.equal((await videoState(mini)).osServices.videoError, undefined, 'the Mini\'s service video error');
      const decoded = (await mini.frames()) - frames;
      t.diagnostic(`${decoded} frames decoded in the 5 s after the mount`);
      assert.ok(decoded > 30, `the browser decoded ${decoded} frames in the 5 s after the mount`);
      assert.equal((await host.usb(mini)).device.devnum, before.device, 'the host\'s USB device number of the Mini');

      // The host reads the new medium: the image's first 64 KiB, past its cache.
      const start = await fetch(server.url, { headers: { Range: 'bytes=0-65535' }, signal: AbortSignal.timeout(30_000) });
      assert.equal(start.status, 206, 'the image\'s server answers a request for a range');
      const expected = createHash('sha256').update(Buffer.from(await start.arrayBuffer())).digest('hex');
      const { device } = await until(medium, now => now?.type === 'rom', 30);
      await until(async () => (await host.exec(`sudo -n ${dd} if=${device} bs=2048 count=32 iflag=direct 2>/dev/null | sha256sum | cut -d" " -f1`)).trim(),
        sum => sum === expected, 30);
    } finally {
      moving = false;
      await moved;
      await mini.rpc('unmountImage', {}, 30_000).catch(() => undefined);
    }
  });
});
