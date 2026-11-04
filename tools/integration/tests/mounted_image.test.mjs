import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import http from 'node:http';
import { test } from 'node:test';
import { setTimeout as sleep } from 'node:timers/promises';
import { sha256 } from '../../combined_load/runner/content.mjs';
import { addressTowards } from '../../combined_load/runner/switches.mjs';
import { hostAgent } from '../lib/host.mjs';
import { spaceReachesHost } from '../lib/keys.mjs';
import { connect, until } from '../lib/mini.mjs';
import { updateServer } from '../lib/servers.mjs';
import { hostTestUsb } from '../lib/usb.mjs';
import { MAX_FREEZE_MS, hdmiCapture, watchVideo } from '../lib/video.mjs';

const INPUT_DEVICES = ['keyboard', 'absolute_mouse', 'relative_mouse'];
const MIB = 1024 * 1024;
// The Mini's drives on the host, by their SCSI type: the device names.
const drives = type => `lsblk -Snpo NAME,TRAN,TYPE,VENDOR 2>/dev/null | awk '$2=="usb" && $3=="${type}" && $4=="JetKVM" {print $1}'`;

// Setup for a mount test on a Linux host. Cleanup runs `undo`, restores USB
// and deletes `files` from the TF card.
async function bench(t, files = []) {
  const host = await hostAgent(t);
  if (!host) return null;
  const mini = await connect();
  const undo = [];
  let restoreUsb = null;
  t.after(async () => {
    try {
      for (const step of undo.reverse()) await step().catch(() => undefined);
      await restoreUsb?.();
    } finally {
      try {
        for (const filename of files.flatMap(name => [name, `${name}.incomplete`])) {
          await mini.rpc('deleteStorageFile', { filename }).catch(e => {
            if (!/file does not exist/.test(e.message)) throw e;
          });
        }
      } finally {
        await mini.close();
      }
    }
  });
  restoreUsb = await hostTestUsb(mini);
  await hdmiCapture(mini);
  await host.inputDevices(INPUT_DEVICES, 30);
  return { host, mini, undo };
}

// The video watch of a test, from now on; the video runs. Returns the end
// of the test's watch, which fails when the video stood still.
async function watch(t, mini) {
  await until(mini.frames, count => count > 0, 30);
  const from = (await mini.frames()) ?? 0;
  // After an upload: a Wi-Fi Mini holds video off while it takes one.
  await until(mini.frames, count => count > from, 30);
  const video = watchVideo(mini);
  t.after(video);
  return () => {
    const freeze = video();
    t.diagnostic(`longest time without a video frame ${freeze.ms} ms`);
    assert.ok(freeze.ms <= MAX_FREEZE_MS, `the video stood still for ${freeze.ms} ms`);
  };
}

// One file on the TF card, as the page uploads it: the start call and one POST.
async function upload(mini, filename, data) {
  const { dataChannel } = await mini.rpc('startStorageFileUpload', { filename, size: data.length });
  const response = await fetch(new URL(`/storage/upload?uploadId=${encodeURIComponent(dataChannel)}`, mini.url), {
    method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: data, signal: AbortSignal.timeout(60_000),
  });
  assert.equal(response.status, 200, `the upload of ${filename}: ${await response.text()}`);
}

// The host has one disk of the Mini with the size of `medium`, and reads
// these bytes from it, within 45 seconds. The read goes through the host's
// cache, as a program's does: a host that missed the change of the medium
// would give the bytes that it has kept.
function hostReads(host, medium, what) {
  const read = `d=$(${drives('disk')}); [ -n "$d" ] || exit 0; s=$(sudo -n blockdev --getsize64 "$d") || exit 0; ` +
    `[ "$s" = ${medium.length} ] || { echo "$s"; exit 0; }; echo "$s $(sudo -n head -c "$s" "$d" | sha256sum | cut -d" " -f1)"`;
  return until(async () => (await host.exec(read, 45_000)).trim(), found => found === `${medium.length} ${sha256(medium)}`, 45)
    .catch(e => assert.fail(`${what} (size and SHA-256 of the Mini's disk on the host) within 45 seconds: ${e.message}`));
}

// The page's "Mount URL" for an address that answers 404.
test('an image URL that is not available is refused, and nothing is mounted', { timeout: 120_000 }, async t => {
  const found = await bench(t);
  if (!found) return;
  const { host, mini, undo } = found;
  const address = await addressTowards(new URL(mini.url).hostname);
  const server = http.createServer((request, response) => response.writeHead(404).end());
  await new Promise(resolve => server.listen(0, address, resolve));
  undo.push(async () => {
    server.closeAllConnections();
    server.close();
  });
  // The host has opened the keyboard that it lists.
  assert.ok((await spaceReachesHost(mini, host, 15)).length > 0, 'the host gets a key before the test');
  const videoRan = await watch(t, mini);
  const startedAt = Date.now();

  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'nothing is mounted before the call');
  const key = mount => `${mount.device}|${mount.mount_point}`;
  const before = new Set((await host.get('/mounts')).map(key));

  await assert.rejects(mini.rpc('mountWithHTTP', { url: `http://${address}:${server.address().port}/missing-image.iso`, mode: 'CDROM' }, 30_000),
    /The URL is not available/);
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'nothing is mounted after the refusal');
  await until(() => host.get('/mounts'), mounts => mounts.every(mount => before.has(key(mount))), 5)
    .catch(e => assert.fail(`the host has no new mounted file system within 5 seconds: ${e.message}`));

  videoRan();
  assert.ok(Date.now() - startedAt <= 30_000, 'the test takes 30 seconds at most');
});

// Same-mode swaps keep the descriptors, so only the Mini can signal a new
// medium. Reading a sector every 20 ms provokes port resets; unmount and
// mount are sent together so no read sees an empty drive.
test('the host reads the new image after one image replaces another in the same mode', { timeout: 600_000 }, async t => {
  const stamp = Date.now();
  const images = [1, 2].map(mib => ({ filename: `integration-remount-${mib}m-${stamp}.img`, data: randomBytes(mib * MIB) }));
  const found = await bench(t, images.map(image => image.filename));
  if (!found) return;
  const { host, mini, undo } = found;
  const startedAt = Date.now();
  for (const image of images) await upload(mini, image.filename, image.data);
  const videoRan = await watch(t, mini);

  await mini.rpc('mountWithStorage', { filename: images[0].filename, mode: 'Disk' }, 60_000);
  await hostReads(host, images[0].data, `the host reads ${images[0].filename} after the first mount`);

  // The reads end by themselves after 300 seconds.
  const reads = (await host.exec(
    'sudo -n timeout 300 sh -c \'while :; do for d in /dev/disk/by-id/usb-JetKVM*; do ' +
    '[ -b $d ] && timeout 2 dd if=$d of=/dev/null bs=2048 count=1 iflag=direct; done; ' +
    'sleep 0.02; done\' < /dev/null > /dev/null 2>&1 & echo $!')).trim();
  assert.match(reads, /^\d+$/, 'the host reads the Mini\'s disk every 20 ms');
  undo.push(() => host.exec(`sudo -n kill ${reads}`));

  for (let cycle = 1; cycle <= 6; cycle++) {
    const image = images[cycle % 2];
    await Promise.all([
      mini.rpc('unmountImage', {}, 60_000),
      mini.rpc('mountWithStorage', { filename: image.filename, mode: 'Disk' }, 60_000),
    ]);
    await hostReads(host, image.data, `change ${cycle}: the host reads ${image.filename}`);
  }

  videoRan();
  assert.ok(Date.now() - startedAt <= 360_000, 'the test takes 360 seconds at most');
});

// The host sees whole blocks of 512 bytes, and the last one ends in zeros.
test('a disk image whose size is not a whole number of 512-byte blocks mounts, and the host reads it', { timeout: 300_000 }, async t => {
  const filename = `integration-unaligned-${Date.now()}.img`;
  const found = await bench(t, [filename]);
  if (!found) return;
  const { host, mini } = found;
  const startedAt = Date.now();
  const data = randomBytes(MIB + 74);
  const medium = Buffer.concat([data, Buffer.alloc(512 - (data.length % 512))]);
  await upload(mini, filename, data);
  const videoRan = await watch(t, mini);

  await mini.rpc('mountWithStorage', { filename, mode: 'Disk' }, 60_000);
  await hostReads(host, medium, 'the host reads the image and zeros up to the end of the last block');

  videoRan();
  assert.ok(Date.now() - startedAt <= 60_000, 'the test takes 60 seconds at most');
});

// PREVENT MEDIUM REMOVAL (eject -i on) stands in for a disc mounted on the
// host. Must stay the last test: it leaves the drive in CD-ROM mode.
test('an unmount succeeds while the host holds the CD-ROM drive locked', { timeout: 300_000 }, async t => {
  const found = await bench(t);
  if (!found) return;
  const { host, mini, undo } = found;
  const server = await updateServer(mini);
  undo.push(async () => server.close());
  const videoRan = await watch(t, mini);
  const startedAt = Date.now();
  const hostGetsKey = async what => assert.ok((await spaceReachesHost(mini, host, 30)).length > 0, `${what} within 30 seconds`);

  await hostGetsKey('the host gets a key before the mount');
  await mini.rpc('mountWithHTTP', { url: server.url, mode: 'CDROM' }, 60_000);
  assert.equal((await mini.rpc('getVirtualMediaState'))?.mode, 'CDROM');
  // The host enumerates the Mini again after the mount.
  await sleep(5000);
  const drive = (await until(
    () => host.exec(`for d in $(${drives('rom')}); do s=$(sudo -n blockdev --getsize64 "$d" 2>/dev/null) && [ "\${s:-0}" -gt 0 ] && echo "$d"; done; true`),
    out => out.trim() !== '', 45).catch(e => assert.fail(`the host has the Mini's CD-ROM drive with a medium within 45 seconds: ${e.message}`)))
    .trim().split('\n')[0];
  await host.exec(`sudo -n eject -i on ${drive}`);
  // The drive can be gone, or another one, after the unmount.
  undo.push(() => host.exec(`sudo -n eject -i off ${drive} 2>/dev/null; true`));

  await mini.rpc('unmountImage', {}, 60_000);
  assert.equal(await mini.rpc('getVirtualMediaState'), null, 'nothing is mounted after the unmount');
  await host.inputDevices(INPUT_DEVICES, 15);
  await hostGetsKey('the host gets a key after the unmount');
  // The keyboard alone does not show it: the drive's first SCSI command can
  // get no answer after a new enumeration, and the host then has no drive.
  await until(() => host.exec(drives('rom')), out => out.trim() !== '', 15)
    .catch(e => assert.fail(`the host has the Mini's drive again within 15 seconds: ${e.message}`));

  videoRan();
  assert.ok(Date.now() - startedAt <= 120_000, 'the test takes 120 seconds at most');
});
