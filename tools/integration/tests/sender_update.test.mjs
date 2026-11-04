import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import { after, before, describe, test } from 'node:test';
import { SENDER_DIRECTORY } from '../../combined_load/runner/hosts.mjs';
import { RELEASES_URL } from '../../combined_load/runner/switches.mjs';
import { SENDER_NOT_APPROVED, SENDER_PROTOCOL_ERROR, senderHost } from '../lib/sender_host.mjs';
import { connect, mockCloudBuild, until } from '../lib/mini.mjs';
import { osServicesParts } from '../../combined_load/runner/os_services.mjs';
import { pageCall } from '../../combined_load/runner/page.mjs';
import { connected, connectedWithClipboard, hook, openSession, videoState } from '../lib/os_services.mjs';
import { releaseBucket } from '../lib/servers.mjs';

// The OS Services protocol: update.chunk carries at most 65,524 bytes.
const CHUNK_BYTES = 65_524;
const HASH_ERROR = 'the package does not match the release index';
const UPDATE_SECONDS = 900;
// A list of stable releases that ends at 0.4.3, with its signature by the release key.
const STABLE_BEFORE_DEV = new URL('../fixtures/stable-0.4.3/', import.meta.url);
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const only = (object, ...names) => Object.fromEntries(names.map(name => [name, object?.[name]]));

// Installs the second-newest sender on the host and has the Mini update it
// from releaseBucket(); restores the host's sender afterwards. Skipped on macOS.
describe('the Mini updates the OS Services sender on the host', () => {
  let mini;
  let host;
  let session;
  let bucket;
  let skip;
  // A host or a build that the tests are not for skips each of them.
  const step = (name, options, run) => test(name, options, t => (skip ? t.skip(skip) : run(t)));
  // The sender's release that the host had, the one before the newest and
  // the newest, each as { version, path, size, sha256 } of the host's package.
  let found;
  let previous;
  let newest;
  let pkg;
  // The releases that the tests installed on the host.
  const installed = [];
  // The start of the update that installs: this machine's clock and the host's.
  let updateSince;
  let updateLogSince;

  // A release of a list as { version, path, size, sha256 } of the host's package.
  const packageOf = release => {
    const file = release?.packages[host.commands.senderPackage];
    return file && { version: release.version, ...file };
  };
  const install = release => host.exec(host.commands.installSender({
    url: `${RELEASES_URL}/${release.path}`, sha256: release.sha256, file: release.path.split('/').pop(),
    fingerprint: session.fingerprint,
  }), { timeoutMs: 600_000 });
  const updates = async since => (await mini.timedEvents('videoInputState'))
    .filter(event => event.at >= since && event.params.guestUpdate !== undefined)
    .map(event => ({ at: event.at, update: event.params.guestUpdate }));
  const requests = (path, since) => bucket.requests.filter(request => request.path === `/os-services/${path}` && request.at >= since);

  // The host gets the sender that it had, and the Mini its settings.
  const putBack = async () => {
    const errors = [];
    const step = async run => run().catch(error => errors.push(error));
    if (session) {
      // An update that is still under way ends first.
      await until(() => videoState(mini), state => !['downloading', 'sending', 'installing'].includes(state.guestUpdate?.phase), 200)
        .catch(() => undefined);
      if (installed.length) {
        await step(async () => {
          await install(found);
          // On Linux the update overwrites the installed release's cached
          // file; delete it if its checksum no longer matches.
          for (const release of host.system === 'linux' ? installed : []) {
            await host.exec(SENDER_DIRECTORY + `f="$dir/${release.path.split('/').pop()}"; ` +
              `echo "${release.sha256}  $f" | sha256sum -c --status 2>/dev/null || rm -f "$f"`, { timeoutMs: 60_000 });
          }
          await host.exec(host.commands.startSender, { timeoutMs: 60_000 });
          const state = await until(() => videoState(mini), now => connected(now) && Boolean(now.osServices.sender?.version), 60);
          assert.equal(state.osServices.sender.version, found.version, 'the host has the sender that it had');
          installed.length = 0;
        });
      }
      await step(() => session.restore());
      session = undefined;
    }
    if (mini) await step(() => mini.close());
    mini = undefined;
    bucket?.close();
    bucket = undefined;
    if (errors.length) throw new AggregateError(errors, `not as found: ${errors.map(error => error.message).join('; ')}`);
  };

  before(async () => {
    try {
      mini = await connect();
      host = await senderHost(mini);
      if (host.system === 'macos') {
        skip = `${host.login} runs macOS, where this update of the sender is not proven`;
        await mini.close();
        mini = undefined;
        return;
      }
      // Another build would install from the real bucket, whatever this
      // test serves.
      const { version: firmware } = (await mini.status()).firmware;
      if (!mockCloudBuild(firmware)) {
        skip = `the Mini runs ${firmware}, which is not a mock cloud build: set JETKVM_MOCK_CLOUD_JWKS_URL in the bench file`;
        await mini.close();
        mini = undefined;
        return;
      }
      const { releases } = await (await fetch(`${RELEASES_URL}/releases.json`, { signal: AbortSignal.timeout(30_000) })).json();
      const release = version => packageOf(releases.find(each => each.version === version));
      previous = packageOf(releases.at(-2));
      newest = packageOf(releases.at(-1));
      assert.ok(previous && newest, `two releases with a package ${host.commands.senderPackage}`);

      session = await openSession(mini, host);
      const version = session.state.osServices.sender.version;
      found = release(version);
      assert.ok(found, `the sender ${version} of ${host.login} is a release, which the test can install again`);

      bucket = await releaseBucket(mini);
      pkg = await bucket.file(newest.path);
      assert.equal(sha256(pkg), newest.sha256, `the SHA-256 of ${newest.path}`);
      // The checks after the update need video, the terminal and file drops.
      await mini.rpc('setOsServicesSettings', { settings: { video_enabled: true, terminal_enabled: true, files_enabled: true } });
      // Each test starts its update itself, and the Mini offers the stable release.
      await mini.rpc('setAutoUpdateState', { enabled: false });
      await mini.rpc('setDevChannelState', { enabled: false });
      await mini.rpc('setUpdateApiUrl', { url: bucket.base });

      installed.push(previous);
      await install(previous);
      // The Mini reads the releases when the sender's session opens.
      await host.exec(host.commands.startSender, { timeoutMs: 60_000 });
      const offered = await until(() => videoState(mini),
        state => connected(state) && state.osServices.sender?.version === previous.version && state.guestUpdate?.version === newest.version, 120);
      assert.deepEqual(only(offered.guestUpdate, 'available', 'installable', 'automatic'), { available: true, installable: true, automatic: false },
        `the Mini offers ${newest.version} to the sender ${previous.version}`);
      // The old sender sends service video, so video that is missing after
      // the update is the update's doing.
      const video = await until(() => videoState(mini),
        state => state.osServices.videoRegistered || state.osServices.videoError !== undefined, 30);
      assert.equal(video.osServices.videoRegistered, true, `service video from ${previous.version}`);
    } catch (error) {
      await putBack().catch(() => undefined);
      throw error;
    }
  }, { timeout: 1_500_000 });

  after(() => putBack(), { timeout: 1_200_000 });

  step('a changed last byte is cancelled before the last chunk; the old sender keeps running',
    { timeout: (UPDATE_SECONDS + 120) * 1000 }, async t => {
      const corrupt = Buffer.from(pkg);
      corrupt[corrupt.length - 1] ^= 0xff;
      bucket.put(newest.path, corrupt);
      const { pid } = await host.sender();
      const logSince = await host.now();
      const since = Date.now();

      await mini.rpc('guestUpdateStart');
      const failed = await until(() => videoState(mini), state => state.guestUpdate?.phase === 'failed', UPDATE_SECONDS);

      // A sender that got every byte would have answered with its own error.
      assert.equal(failed.guestUpdate.error, HASH_ERROR, 'the Mini\'s verdict');
      const lastChunk = pkg.length % CHUNK_BYTES || CHUNK_BYTES;
      assert.ok(failed.guestUpdate.sent <= pkg.length - lastChunk,
        `the sender got ${failed.guestUpdate.sent} bytes of ${pkg.length}, and the last chunk has ${lastChunk}`);
      const phases = (await updates(since)).map(event => event.update.phase);
      assert.ok(phases.includes('sending'), `the Mini reported "sending": ${JSON.stringify([...new Set(phases)])}`);
      assert.ok(!phases.includes('installing'), 'the Mini never reported "installing"');
      assert.equal(requests(newest.path, since).length, 1, 'downloads of the package');

      assert.equal((await host.sender()).pid, pid, 'the sender\'s process');
      const state = await videoState(mini);
      assert.equal(connected(state), true, 'the session stays open');
      assert.equal(state.osServices.sender?.version, previous.version, 'the sender\'s version');
      assert.deepEqual((await host.senderLog(logSince)).filter(entry => SENDER_PROTOCOL_ERROR.test(entry.message))
        .map(entry => entry.message), [], 'protocol errors in the sender\'s log');
    });

  // Starts the update to `target` and checks it to its end: the phases and
  // the progress that the Mini reports, the one download, the new program
  // in a new process and the version that the sender reports. The Mini
  // fetches the signature of a package of the signed list alone.
  const updatesTo = async (t, target, signed) => {
    const before = { ...await host.sender(), programSum: await host.programSum() };
    updateLogSince = await host.now();
    updateSince = Date.now();

    await mini.rpc('guestUpdateStart');
    // The sender's details follow the session that ends the update by one
    // change of the state.
    const ended = await until(() => videoState(mini),
      state => (state.guestUpdate?.phase === 'done' && Boolean(state.osServices.sender)) || state.guestUpdate?.phase === 'failed',
      UPDATE_SECONDS);

    assert.deepEqual(only(ended.guestUpdate, 'phase', 'version', 'available', 'error'),
      { phase: 'done', version: target.version, available: false, error: undefined }, 'the Mini\'s update state');
    assert.equal(ended.osServices.sender?.version, target.version, 'the version that the sender reports');
    // The sender's service runs the new program in a new process. The Linux
    // package is the program itself.
    const now = { ...await host.sender(), programSum: await host.programSum() };
    assert.equal(now.running, true, 'the sender runs');
    assert.notEqual(now.pid, before.pid, 'the sender\'s process is a new one');
    assert.notEqual(now.programSum, before.programSum, 'the sender\'s program was replaced');
    if (host.system === 'linux') assert.equal(now.programSum, target.sha256, 'the sender\'s program is the package');

    const seen = await updates(updateSince);
    // Before the start the Mini can still report the end of the update before.
    const phases = [...new Set(seen.map(event => event.update.phase))];
    assert.deepEqual(phases.slice(phases.indexOf('sending')), ['sending', 'installing', 'done'], 'phases in order');
    // The Mini publishes progress at most once a second; the last step, to
    // the whole package, goes out at once.
    const progress = seen.filter((event, i) => event.update.phase === 'sending' &&
      event.update.sent !== seen[i - 1]?.update.sent && event.update.sent !== target.size);
    // On Wi-Fi one event can arrive hundreds of milliseconds late, so single
    // gaps say little. Counted over the whole transfer, once a second allows
    // one event per second of span, plus two for the delays of the first and
    // the last; twice a second would exceed it.
    const span = progress.length ? progress.at(-1).at - progress[0].at : 0;
    assert.ok(progress.length - 1 <= Math.floor(span / 1000) + 2, `${progress.length} progress steps in ${span} ms`);
    // And progress is reported: at least one step every 2 s.
    const sending = seen.find(event => event.update.phase === 'sending').at;
    const installing = seen.find(event => event.update.phase === 'installing').at;
    assert.ok(progress.length >= Math.floor((installing - sending) / 2000),
      `${progress.length} progress steps in ${installing - sending} ms of sending`);
    assert.equal(requests(target.path, updateSince).length, 1, 'downloads of the package');
    assert.equal(requests(`${target.path}.sig`, updateSince).length, signed ? 1 : 0, 'requests for the package\'s signature');
    t.diagnostic(`${target.version} (${target.size} bytes) sent in ${installing - sending} ms, ` +
      `installed ${seen.find(event => event.update.phase === 'done').at - installing} ms later`);
  };

  step('the sender installs the release and reports its version', { timeout: (UPDATE_SECONDS + 120) * 1000 }, async t => {
    bucket.put(newest.path, pkg);
    await updatesTo(t, newest, true);
  });

  step('the updated sender sends video and serves the clipboard, a terminal and files', { timeout: 300_000 }, async t => {
    assert.equal((await videoState(mini)).osServices.sender?.version, newest.version,
      `the sender runs ${newest.version}, which the test before installs`);
    // Service video is back, and the browser decodes new frames of it: the
    // pointer moves, so the sender has new frames to send.
    const [width, height] = (await mini.rpc('getOsServicesSettings')).resolution.split('x').map(Number);
    const showing = await until(() => videoState(mini), state => state.osServices.videoError !== undefined ||
      (state.source === 'guest' && state.osServices.video && state.ready && state.width === width && state.height === height), 60);
    assert.equal(showing.osServices.videoError, undefined, 'the Mini\'s service video error');
    await until(() => hook(mini, 'getVideoStreamDimensions'),
      size => size?.width === width && size?.height === height, 30);
    assert.equal(showing.osServices.videoRegistered, true, 'service video');
    const decoded = await mini.frames() ?? 0;
    await until(async () => {
      await mini.rpc('absMouseReport', { x: Math.floor(Math.random() * 32768), y: Math.floor(Math.random() * 32768), buttons: 0 });
      return mini.frames();
    }, frames => frames > decoded, 15);
    assert.equal((await videoState(mini)).source, 'guest', 'the video source');

    // The new sender found the KVM in its approved list and never asked.
    assert.equal(connected(await videoState(mini)), true, 'the session is open');
    const asked = (await mini.timedEvents('videoInputState'))
      .filter(event => event.at >= updateSince && event.params.osServices?.approvalPending);
    assert.equal(asked.length, 0, 'approval prompts since the update');
    assert.deepEqual((await host.senderLog(updateLogSince)).filter(entry => SENDER_NOT_APPROVED.test(entry.message))
      .map(entry => entry.message), [], 'the sender\'s refusals since the update');

    // The text goes to the sender and back.
    await until(() => videoState(mini), connectedWithClipboard, 30);
    const text = `after the update ${Date.now()} ✓`;
    assert.deepEqual(await mini.rpc('guestClipboardSet', { text }, 15_000), { status: 'ok' });
    assert.deepEqual(await mini.rpc('guestClipboardGet', {}, 15_000), { status: 'ok', text, truncated: false });

    // The walk's terminal, in which the host counts up, and its file
    // transfer, which checks each file's SHA-256 on the host and removes it.
    await until(() => videoState(mini), state => state.guestTerminal === true && state.guestFile?.receive === true, 30);
    const { osTerminal, osFiles } = osServicesParts({ page: mini.page, call: pageCall(mini.page), hostExec: host.exec, host: host.commands });
    await osTerminal.on();
    try {
      await until(() => osTerminal.works(true), Boolean, 30);
      assert.deepEqual((await osTerminal.sample()).breaks, [], 'numbers that the terminal left out');
    } finally {
      await osTerminal.off();
    }
    await osFiles.on();
    let files;
    try {
      files = await until(() => osFiles.sample(), sample => sample && sample.verified + sample.failed > 0, 60);
    } finally {
      await osFiles.off();
      // The channel of the transfer that the stop cut off has closed.
      await sleep(1000);
    }
    assert.deepEqual(only(files, 'failed', 'refused'), { failed: 0, refused: 0 }, `file transfers: ${JSON.stringify(files)}`);
  });
  // The dev list has no signatures. The Mini offers its newest release when
  // that is newer than the newest of the signed list, so the stand-in serves
  // a signed list that ends before the dev release (fixtures/stable-0.4.3).
  step('on the dev channel the sender installs the release of the unsigned dev list', { timeout: (UPDATE_SECONDS + 900) * 1000 }, async t => {
    const stable = fs.readFileSync(new URL('releases.json', STABLE_BEFORE_DEV));
    const base = packageOf(JSON.parse(stable).releases.at(-1));
    const { releases } = JSON.parse(await bucket.file('releases-dev.json'));
    const dev = packageOf(releases.at(-1));
    assert.ok(base && dev, `a release of the fixture and a dev release with a package ${host.commands.senderPackage}`);
    // The stand-in has the package before the Mini asks for it.
    assert.equal(sha256(await bucket.file(dev.path)), dev.sha256, `the SHA-256 of ${dev.path}`);
    // The Mini keeps the lists that it has read for up to an hour, and a
    // restart makes it read them again: before the test, so that it reads
    // the fixture, and after it, so that it does not keep the fixture.
    const restart = async () => {
      const asked = Date.now();
      mini.rpc('reboot', { force: true }, 5000).catch(() => undefined);
      await until(() => mini.restarted(asked), Boolean, 120);
      await mini.open();
    };
    const real = { 'releases.json': await bucket.file('releases.json'), 'releases.json.sig': await bucket.file('releases.json.sig') };
    t.after(async () => {
      for (const [path, bytes] of Object.entries(real)) bucket.put(path, bytes);
      await restart();
    });
    bucket.put('releases.json', stable);
    bucket.put('releases.json.sig', fs.readFileSync(new URL('releases.json.sig', STABLE_BEFORE_DEV)));

    installed.push(base);
    await install(base);
    await mini.rpc('setDevChannelState', { enabled: true });
    await restart();
    // The Mini reads both lists when the sender's session opens.
    const since = Date.now();
    await host.exec(host.commands.startSender, { timeoutMs: 60_000 });
    const offered = await until(() => videoState(mini),
      state => connected(state) && state.osServices.sender?.version === base.version && state.guestUpdate?.version === dev.version, 120);
    assert.deepEqual(only(offered.guestUpdate, 'available', 'installable', 'automatic'), { available: true, installable: true, automatic: false },
      `the Mini offers ${dev.version} to the sender ${base.version}`);
    assert.ok(requests('releases.json.sig', since).some(request => request.status === 200), 'the Mini fetched the signature of the signed list');
    assert.ok(requests('releases-dev.json', since).length > 0, 'the Mini fetched the dev list');
    assert.equal(requests('releases-dev.json.sig', since).length, 0, 'requests for a signature of the dev list');

    await updatesTo(t, dev, false);
  });
});
