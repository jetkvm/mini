// node --test tools/combined_load/runner/combined_load.test.mjs
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import os from 'node:os';
import path from 'node:path';
import { X509Certificate, createHash, createPrivateKey, createPublicKey, verify } from 'node:crypto';
import net from 'node:net';
import { test } from 'node:test';
import tls from 'node:tls';
import { fileURLToPath } from 'node:url';
import { benchBuildArguments, benchVersion, outputInCheckout } from './build.mjs';
import { beepClock, beepDelayMs, beepDetector, levelBetween, pageSound, playsTrack, toneFinder } from './audio.mjs';
import { SETTLE_CAP_SECONDS, SETTLE_CONFIRM_SECONDS, drive, settledAt, until, validateList, withoutVariables } from './driver.mjs';
import { EXIT, changeLimit, changeLine, expand, judge, readJsonc, report, secondsNotNormal, toolVersion, toolVersionNotes, validateCriteria, validateKnown } from './judge.mjs';
import { RELEASES_URL, VARIABLES, adopt, checkSenderType, mediumOnHost, noisePictureOn, otherMode, parseSize, readFirmware, senderOverride, senderToInstall, serve, updateCheckSwitch } from './switches.mjs';
import { SESSION_MS, SOCKET_MS, USE_HERE_SHOWN_MS, firstFrame, signalingSockets, takeoverSwitch } from './takeover.mjs';
import { serverCertificate, startFakeCloud } from './fake_cloud.mjs';
import { hidTimes, hostInputAt, hostKeys, hostMoves, keyAnswers, moveHand, parseReport, watchKeyStates } from './hid_times.mjs';
import { BASE_TOPIC, mqttSwitch } from './mqtt.mjs';
import { tailscaleSwitch } from './tailscale.mjs';
import { IMAGE_BLOCK_BYTES, blockChecksums, imageBytes, seededBytes, sha256 } from './content.mjs';
import { HID_KEYS, PYTHON_CHECK_INPUT, hostFor } from './hosts.mjs';
import { osServicesParts, startFiles, startTerminal, takeRoundTrips } from './os_services.mjs';
import { ALL_LOST, DROP_MS, NO_LOSS, packetLossSwitch } from './packet_loss.mjs';
import { ALLOCATION_FAILURES, frameTimes, fullStatsDue, fullStatsPause, memoryCounters, pauseNow, routeAddress, startRecorder, taskStacksRow, toolLink, withLate } from './recorder.mjs';
import { SERVICE_VIDEO, cdromDrive, setDefaults, settingLines } from './settings.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const T0 = 1_000_000;

// A recording of one sample per second. Each argument overrides one field
// for the seconds it names: frozen (no new decoded frames), noDevice (no
// stats response), noPeer (the page has no peer connection), restartAt (the
// device's clock starts again), noRead (the host's CD-ROM is gone).
function recording(seconds, state, { frozen = [], noDevice = [], noPeer = [], restartAt = null, noRead = [] } = {}) {
  const rows = [];
  let frames = 0;
  let bytesRead = 0;
  for (let second = 0; second < seconds; second++) {
    if (!frozen.includes(second)) frames += 30;
    bytesRead += 100_000;
    const stateNow = typeof state === 'function' ? state(second) : state;
    rows.push({
      t: T0 + second * 1000,
      state: stateNow,
      device: noDevice.includes(second) ? null
        : { nowMs: restartAt !== null && second >= restartAt ? (second - restartAt) * 1000 : 60_000 + second * 1000 },
      browser: noPeer.includes(second) ? null : { connectionState: 'connected', video: { framesDecoded: frames } },
      loads: stateNow.mount ? { mount: noRead.includes(second) ? null : { bytesRead } } : {},
    });
  }
  return rows;
}
// A change that starts at `second` and whose switch takes `seconds`.
const change = (variable, on, second, seconds = 1, { error = null, state = {} } = {}) => ({
  t: T0 + (second + seconds) * 1000, change: { [variable]: on }, state: { ...state, [variable]: on },
  startedAt: T0 + second * 1000, seconds, switchSeconds: seconds, error,
});
const range = (from, to) => Array.from({ length: to - from + 1 }, (_, i) => from + i);

// The rows with hid's events in its samples: each send of a key [usage,
// sentAt], each move [x, sentAt, movement] and each line of the host's watch
// [what, hostMs, readAt] in the sample of the second in which it happened.
// The first sample has every event up to its time.
const withHid = (rows, { sends = [], moves = [], reports = [] }) => rows.map((row, index) => {
  const inSecond = at => at <= row.t && (index === 0 || at > row.t - 1000);
  return { ...row, loads: { ...row.loads, hid: { sends: sends.filter(([, at]) => inSecond(at)), keys: [],
    moves: moves.filter(([, at]) => inSecond(at)), reports: reports.filter(([, , at]) => inSecond(at)) } } };
});

// The sample of osClipboard of a row with one round of 60 ms that ended in its second: back is true when the
// text came back as it was set, false when the host answered with another text, null without an answer.
const clipboardLoad = (row, back = true) => ({ rounds: 0, wrong: 0, corrupt: 0, ended: [[row.t - 500, 60, back]], runningSince: null, at: row.t });

const VIDEO = { while: { video: true }, measure: 'framesPerSecond', min: 1, failAfterSeconds: 2 };
const rules = (...everySecond) => ({ everySecond });

test('a breach of failAfterSeconds passes; one second more fails once, with its state and last change', () => {
  assert.equal(judge(recording(20, { video: true }, { frozen: [5, 6] }), rules(VIDEO)).result, 'pass');

  const rows = [change('mount', true, 3), ...recording(20, { video: true, mount: true }, { frozen: range(6, 12) })];
  const { result, failures } = judge(rows, rules(VIDEO));
  assert.equal(result, 'fail');
  assert.equal(failures.length, 1);
  assert.equal(failures[0].t, T0 + 8000);
  assert.deepEqual(failures[0].state, { video: true, mount: true });
  assert.deepEqual(failures[0].lastChange.change, { mount: true });
});

test('a breach ends when its rule no longer applies', () => {
  // Two bad seconds with video on, three seconds with it off, one bad second with it on again.
  const rows = recording(10, second => ({ video: second < 4 || second > 6 }), { frozen: [2, 3, 7] });
  assert.equal(judge(rows, rules(VIDEO)).result, 'pass');
});

test('a breach ends in a sample in which the Mini answered and the measure has no value, and goes on across a sample without the answer', () => {
  // Two slow seconds at 5 and 6, under the rule's failAfterSeconds, then seconds in which the Mini encodes no
  // HDMI frame, then one more slow second.
  const rule = rules({ while: { video: true }, measure: 'encodeMs', max: 33.3, failAfterSeconds: 2 });
  const slow = [5, 6, 27];
  const rows = noAnswer => withEncoder(recording(30, { video: true }, { noDevice: noAnswer }),
    second => [second > 6 && second < 27 ? 0 : 30, slow.includes(second) ? 34 : 33, 0, 0]);
  assert.equal(judge(rows([]), rule).result, 'pass');
  // Without the Mini's answers in those seconds the three slow seconds are one breach.
  assert.deepEqual(judge(rows(range(7, 26)), rule).failures.map(f => [f.t - T0, f.value]), [[27000, 34]]);
  // The same slow second right after the first two is the third second of their breach.
  const joined = withEncoder(recording(30, { video: true }), second => [30, [5, 6, 7].includes(second) ? 34 : 33, 0, 0]);
  assert.deepEqual(judge(joined, rule).failures.map(f => [f.t - T0, f.value]), [[7000, 34]]);
});

test('a breach of a switch\'s own measure ends when the switch goes off', () => {
  // The Mini reports no key for one second, keys go off for three seconds, and it reports none for one more
  // second after they are on again.
  const rows = recording(10, second => ({ video: true, hid: second < 4 || second > 6 }));
  const keys = second => (second === 3 || second === 8 ? [[0x04, T0 + second * 1000 - 3000, null]] : second > 3 && second < 8 ? [] : [[0x04, T0, 5]]);
  rows.forEach((row, second) => { if (row.state.hid) row.loads = { hid: { keys: keys(second) } }; });
  const rule = { while: { hid: true }, measure: 'keyLatencyMaxMs', max: 500, failAfterSeconds: 1 };
  assert.equal(judge(rows, rules(rule)).result, 'pass');
  // With the keys on, and without a key in the seconds between, the two seconds are one breach.
  const on = recording(10, { video: true, hid: true }).map((row, second) => ({ ...row, loads: { hid: { keys: keys(second) } } }));
  assert.deepEqual(judge(on, rules(rule)).failures.map(f => [f.t - T0, f.value]), [[8000, 3000]]);
});

test('what happens during a pause without a report is not counted after the pause', () => {
  // The browser has no report during the change; its next report has the change's freeze.
  const rows = recording(10, { video: true }, { noPeer: [3, 4] });
  rows.forEach((row, second) => { if (row.browser) row.browser.video.totalFreezesDuration = second >= 5 ? 2 : 0; });
  const criteria = { everySecond: [{ while: { video: true }, measure: 'freezeMs', max: 0 }],
    changeTakesAtMost: [{ change: { source: true }, max: 5, pauses: ['freezeMs'] }] };
  assert.equal(judge([...rows, change('source', true, 2, 2)], criteria).result, 'pass');
});

test('a pause of one measure does not hide what another measure counts', () => {
  // The browser has no report during a change that pauses the frame rate only; a freeze in it counts.
  const rows = recording(10, { video: true }, { noPeer: [3, 4] });
  rows.forEach((row, second) => { if (row.browser) row.browser.video.totalFreezesDuration = second >= 5 ? 2 : 0; });
  const criteria = { everySecond: [{ while: { video: true }, measure: 'freezeMs', max: 0 }],
    changeTakesAtMost: [{ change: { source: true }, max: 5, pauses: ['framesPerSecond'] }] };
  assert.deepEqual(judge([...rows, change('source', true, 2, 2)], criteria).failures.map(f => f.value), [2000]);
});

test('a second breach after a recovery is a second failure', () => {
  const { failures } = judge(recording(30, { video: true }, { frozen: [...range(5, 9), ...range(15, 19)] }), rules(VIDEO));
  assert.deepEqual(failures.map(f => f.t), [T0 + 7000, T0 + 17000]);
});

test('a change pauses the checks it names until it is finished, and no others', () => {
  const criteria = { everySecond: [VIDEO], changeTakesAtMost: [{ change: { osServices: true }, max: 8, pauses: ['framesPerSecond'] }] };
  const rows = (variable, frozen) => [change(variable, true, 10, 4), ...recording(40, { video: true }, { frozen })];
  // No frames for the 4 seconds of the change and the 2 the rule allows.
  assert.equal(judge(rows('osServices', range(11, 16)), criteria).result, 'pass');
  assert.deepEqual(judge(rows('osServices', range(11, 17)), criteria).failures.map(f => f.t), [T0 + 17000]);
  // A mount pauses nothing here, so the same gap fails at its third second.
  assert.deepEqual(judge(rows('mount', range(11, 16)), criteria).failures.map(f => f.t), [T0 + 13000]);
});

test('a pause covers the second in which its change ends', () => {
  const criteria = { everySecond: [{ while: { osTerminal: true }, measure: 'terminalLinesLost', max: 0 }],
    changeTakesAtMost: [{ change: { mount: true }, max: 7, pauses: ['terminalLinesLost'] }] };
  // A line that was lost in the change is counted in the sample after its end.
  const rows = late => [change('mount', true, 10, 1.6), ...recording(20, { osTerminal: true }).map((row, second) =>
    ({ ...row, loads: { osTerminal: { breaks: second === late ? [[row.t - 500, 41, 43]] : [] } } }))];
  assert.equal(judge(rows(12), criteria).result, 'pass');
  assert.deepEqual(judge(rows(13), criteria).failures.map(f => f.t), [T0 + 13000]);
});

test('pauseSeconds keeps a change\'s pause on for that long after its switch is done, and a rule without it ends the pause with the switch', () => {
  const rule = pauseSeconds => ({ everySecond: [{ while: { osTerminal: true }, measure: 'terminalLinesLost', max: 0 }],
    changeTakesAtMost: [{ change: { mount: true }, max: 7, pauses: ['terminalLinesLost'], ...(pauseSeconds === undefined ? {} : { pauseSeconds }) }] });
  // The switch is done at 11.5 s; with 2.5 s more the pause ends at 14.0 s.
  // A line that is lost at 13.5 s (2.0 s after the switch) is in the sample of
  // 14 s, and one lost at 14.5 s (3.0 s after it) is in the sample of 15 s.
  const rows = late => [change('mount', true, 10, 1.5), ...recording(20, { osTerminal: true }).map((row, second) =>
    ({ ...row, loads: { osTerminal: { breaks: second === late ? [[row.t - 500, 41, 43]] : [] } } }))];
  assert.equal(judge(rows(14), rule(2.5)).result, 'pass');
  assert.deepEqual(judge(rows(15), rule(2.5)).failures.map(f => f.t), [T0 + 15000]);
  // Without the field the pause ends with the switch: the line at 13.5 s fails.
  assert.deepEqual(judge(rows(14), rule()).failures.map(f => f.t), [T0 + 14000]);
  // A paused measure keeps no second from being normal: the state settles inside the pause.
  const paused = secondsNotNormal(rows(14), rule(2.5)).find(second => second.t === T0 + 14000);
  assert.deepEqual(paused.notNormal, []);
  assert.ok(!paused.normal.includes('terminalLinesLost'));
  assert.throws(() => validateCriteria({ changeTakesAtMost: [{ change: { mount: true }, max: 7, pauseSeconds: 2 }] }, ['mount']),
    /pauseSeconds/);
});

test('a pause of lost keys reaches as far as the key is judged, and a lost mouse move as far as it is judged', () => {
  const criteria = { everySecond: [{ while: { hid: true }, measure: 'keysLost', max: 0 }, { while: { hid: true }, measure: 'mouseMovesLost', max: 0 }],
    changeTakesAtMost: [{ change: { mount: true }, max: 7, pauses: ['keysLost', 'mouseMovesLost'] }] };
  // The mount ends at 11.8 s. A key that the host did not get is counted
  // 3 s after its send, so the sample of 15 s reports the keys from 11.0 s
  // on, and is paused; the sample of 16 s reports them from 12.0 s on. A
  // move is judged 1.3 s after its send, so the sample of 14 s reports the
  // moves from 11.7 s on, and is paused. The key or the move that the host
  // did not get is counted in the sample of lostAt.
  const rows = (lostAt, measure) => {
    const at = T0 + lostAt * 1000 - 500;
    const events = measure === 'keys' ? { sends: [[0x04, at - 3000]] } : { moves: [[4000, at - 1300, 0]] };
    return [change('mount', true, 10, 1.8), ...withHid(recording(30, { hid: true }), events)];
  };
  assert.equal(judge(rows(15, 'keys'), criteria).result, 'pass');
  assert.equal(judge(rows(14, 'mouse'), criteria).result, 'pass');
  // The same counts in the next sample are of a key or a move after the change.
  assert.deepEqual(judge(rows(16, 'keys'), criteria).failures.map(f => [f.measure, f.t - T0]), [['keysLost', 16_000]]);
  assert.deepEqual(judge(rows(15, 'mouse'), criteria).failures.map(f => [f.measure, f.t - T0]), [['mouseMovesLost', 15_000]]);
});

test('a rule is checked during a change that does not pause it', () => {
  const criteria = rules({ measure: 'deviceRestarts', max: 0 });
  const rows = [change('mount', true, 5, 10), ...recording(20, {}, { restartAt: 8 })];
  assert.deepEqual(judge(rows, criteria).failures.map(f => f.measure), ['deviceRestarts']);
});

test('the "while" rule that names more variables replaces the other', () => {
  const criteria = rules(VIDEO, { while: { video: true, ota: true }, measure: 'framesPerSecond', min: 1, failAfterSeconds: 5 });
  const frozen = range(5, 8);
  assert.equal(judge(recording(20, { video: true, ota: true }, { frozen }), criteria).result, 'pass');
  assert.equal(judge(recording(20, { video: true }, { frozen }), criteria).result, 'fail');
});

test('two "while" rules that apply equally leave the run undecided', () => {
  const lines = [
    { while: { video: true, ota: true }, measure: 'framesPerSecond', min: 1, failAfterSeconds: 5 },
    { while: { video: true, mount: true }, measure: 'framesPerSecond', min: 1, failAfterSeconds: 3 },
  ];
  const { result, undecided } = judge(recording(20, { video: true, ota: true, mount: true }), rules(VIDEO, ...lines));
  assert.equal(result, 'undecided');
  assert.equal(undecided.length, 1);
  assert.deepEqual(undecided[0].lines, lines);
});

const MOUNT_LIMIT = { changeTakesAtMost: [{ change: { mount: true }, max: 10 }] };

test('a change fails on an error or on its limit', () => {
  // No rule judges a second: each state settles when its switch is done.
  const samples = on => recording(20, { mount: on });
  assert.equal(judge([change('mount', true, 0, 4.2), ...samples(true)], MOUNT_LIMIT).result, 'pass');
  assert.equal(judge([change('mount', true, 0, 11), ...samples(true)], MOUNT_LIMIT).result, 'fail');
  assert.equal(judge([change('mount', false, 0, 11), ...samples(false)], MOUNT_LIMIT).result, 'pass');
  const refused = judge([change('mount', true, 0, 0.3, { error: 'Not enough memory to start the download' })], MOUNT_LIMIT);
  assert.equal(refused.failures[0].text, 'Not enough memory to start the download');
});

test('a device that does not answer fails after failAfterSeconds; a restart fails at once', () => {
  const criteria = rules({ measure: 'statsResponsesPerSecond', min: 1, failAfterSeconds: 3 }, { measure: 'deviceRestarts', max: 0 });
  assert.equal(judge(recording(20, {}, { noDevice: range(5, 7) }), criteria).result, 'pass');
  assert.deepEqual(judge(recording(20, {}, { noDevice: range(5, 8) }), criteria).failures.map(f => f.measure),
    ['statsResponsesPerSecond']);
  assert.deepEqual(judge(recording(20, {}, { noDevice: range(5, 6), restartAt: 7 }), criteria).failures.map(f => f.measure),
    ['deviceRestarts']);
});

test('an answer of the Mini that was made after a change began is not judged against the state before the change', () => {
  // The Mini holds video and the sound off for an upload, and reports each.
  for (const [measure, reported] of [['videoPausedByUpload', 'videoPausedBy'], ['audioPausedByUpload', 'audioPausedBy']]) {
    const criteria = rules(
      { measure, max: 0 },
      { while: { upload: true }, measure, min: 1 });
    const uploading = second => second >= 3 && second <= 8;
    // The upload goes off 0.1 s after the sample of second 8. answerAfterMs is
    // how long after its sample's time the Mini made the answer of second 8,
    // and pausedThen what it reports in it.
    const failuresWith = (answerAfterMs, pausedThen, change8 = 'upload') => {
      const rows = recording(12, second => ({ video: true, upload: uploading(second) }));
      rows.forEach((row, second) => { row.device[reported] = uploading(second) && second !== 8 ? ['upload'] : []; });
      rows[8].device[reported] = pausedThen;
      rows[8].device.nowMs += answerAfterMs;
      const off = { ...change(change8, false, 8.1, 0.5), state: { video: true, upload: change8 !== 'upload', [change8]: false } };
      return judge(change8 ? [off, ...rows] : rows, criteria).failures.map(f => [f.t, f.value]);
    };
    // The Mini answered 0.7 s after it was asked, when the upload was going off: no pause, and no failure.
    assert.deepEqual(failuresWith(700, []), [], measure);
    // It answered 50 ms after it was asked, before the change began: that answer is of the upload.
    assert.deepEqual(failuresWith(50, []), [[T0 + 8000, 0]], measure);
    // A late answer without a change in between is judged as it is.
    assert.deepEqual(failuresWith(700, [], null), [[T0 + 8000, 0]], measure);
    // A change of another variable in between leaves the upload's rule as it is: the answer is judged.
    assert.deepEqual(failuresWith(700, [], 'hid'), [[T0 + 8000, 0]], measure);
  }
});

test('the report has the tool machine\'s highest load and names a failure in seconds in which that machine was busy', () => {
  const frozenAt = frozen => {
    const rows = recording(30, { video: true }, { frozen });
    // The load is 9 from second 18 to 24 and 3 otherwise, on 16 cores.
    rows.forEach((row, second) => { row.load = second >= 18 && second <= 24 ? 9 : 3; });
    return report(judge([{ t: T0, run: { board: 'ethernet', host: 'linux', cores: 16 } }, ...rows], rules(VIDEO)), T0).split('\n');
  };
  const quiet = frozenAt([5, 6, 7]);
  assert.equal(quiet[1], 'LOAD  9 at most, the 1-minute load of the machine that ran the tool, which has 16 cores');
  assert.ok(quiet.some(line => line.startsWith('FAIL  framesPerSecond')));
  assert.ok(!quiet.some(line => line.includes('tool machine busy')));
  // The breach starts at second 16; its third second, where it fails, has the load of 9.
  assert.ok(frozenAt([16, 17, 18]).some(line => line.trim() === 'tool machine busy: load 9 on 16 cores'));
  // A breach whose first sample follows the busy seconds is not marked.
  assert.ok(!frozenAt([25, 26, 27]).some(line => line.includes('tool machine busy')));
  // A recording without the load has no such lines.
  const without = report(judge(recording(30, { video: true }, { frozen: [16, 17, 18] }), rules(VIDEO)), T0);
  assert.ok(!/LOAD|busy/.test(without));
});

test('the report has the seconds in which the tool machine was off the network and names a failure in or after them', () => {
  // The interface's link goes at second 12 and comes back at second 20 (its count of changes rises at
  // each), and the interface is without its address from second 18 to 24.
  const frozenAt = frozen => {
    const rows = recording(60, { video: true }, { frozen });
    rows.forEach((row, second) => {
      row.link = { carrier: second < 12 || second >= 20, carrierChanges: 4 + (second >= 12) + (second >= 20), address: second < 18 || second > 24 };
    });
    return report(judge([{ t: T0, run: { board: 'ethernet', host: 'linux', toolInterface: 'enp2s0' } }, ...rows], rules(VIDEO)), T0).split('\n');
  };
  const during = frozenAt(range(12, 25));
  assert.equal(during[1], 'LINK  the machine that ran the tool was off the network at 12 to 24 s: its interface enp2s0 lost its link or its address');
  assert.ok(during.some(line => line.trim() === 'tool machine off the network: 12 to 14 s'));
  // A breach that begins while the session comes back, 6 s after the address, is marked with the whole time.
  assert.ok(frozenAt([30, 31, 32]).some(line => line.trim() === 'tool machine off the network: 20 to 24 s'));
  // A breach that ended before the link went, and one that begins 20 s after the address is back, are not.
  assert.ok(!frozenAt([5, 6, 7]).some(line => line.includes('tool machine off the network')));
  assert.ok(!frozenAt([45, 46, 47]).some(line => line.includes('tool machine off the network')));
  // A link that only came and went between two samples is one second off the network.
  const blink = recording(30, { video: true }, { frozen: [10, 11, 12] });
  blink.forEach((row, second) => { row.link = { carrier: true, carrierChanges: second >= 10 ? 2 : 0, address: true }; });
  assert.ok(report(judge(blink, rules(VIDEO)), T0).split('\n').includes('LINK  the machine that ran the tool was off the network at 10 s'));
  // The count is compared with the newest one before it, also across a sample that has none.
  const gap = recording(30, { video: true }, { frozen: [10, 11, 12] });
  gap.forEach((row, second) => { row.link = second === 10 ? { address: true } : { carrier: true, carrierChanges: second > 10 ? 2 : 0, address: true }; });
  assert.ok(report(judge(gap, rules(VIDEO)), T0).split('\n').includes('LINK  the machine that ran the tool was off the network at 11 s'));
  // A recording of a machine without the link's state (not Linux) is judged by the address alone, and one
  // without the interface's state has no such lines.
  const address = recording(30, { video: true }, { frozen: [10, 11, 12] });
  address.forEach((row, second) => { row.link = { address: second !== 11 }; });
  assert.ok(report(judge(address, rules(VIDEO)), T0).includes('tool machine off the network: 11 s'));
  assert.ok(!/LINK|off the network/.test(report(judge(recording(30, { video: true }, { frozen: [10, 11, 12] }), rules(VIDEO)), T0)));
});

test('the tool reads the state of its network interface towards the Mini', async () => {
  // The system's route to an address of this machine starts at that address, for both families and
  // for an IPv6 address in brackets, as a URL has it. A name that does not resolve has no route.
  assert.deepEqual(await routeAddress('127.0.0.1', 80), { address: '127.0.0.1', family: 4 });
  if (Object.values(os.networkInterfaces()).flat().some(entry => entry.address === '::1')) {
    assert.deepEqual(await routeAddress('[::1]', 80), { address: '::1', family: 6 });
  }
  assert.equal(await routeAddress('no-such-host.invalid', 80), null);
  const v4 = address => ({ address, family: 'IPv4' });
  const v6 = address => ({ address, family: 'IPv6' });
  const system = {
    addresses: { lo: [v4('127.0.0.1')], enp2s0: [v6('fe80::1'), v4('192.168.1.102'), v6('2001:db8::47')], wlp3s0: [] },
    files: { '/sys/class/net/enp2s0/carrier': '1\n', '/sys/class/net/enp2s0/carrier_changes': '40\n' },
  };
  const asked = [];
  const options = {
    route: async (host, port) => {
      asked.push([host, port]);
      return { '192.168.1.100': { address: '192.168.1.102', family: 4 }, '[2001:db8::44]': { address: '2001:db8::47', family: 6 } }[host] ?? null;
    },
    interfaces: () => system.addresses,
    readFile: file => {
      if (!(file in system.files)) throw new Error('ENOENT');
      return system.files[file];
    },
  };
  // The interface is the one with the address from which the route to the Mini starts.
  const link = await toolLink('http://192.168.1.100', options);
  assert.equal(link.name, 'enp2s0');
  assert.deepEqual(link.read(), { carrier: true, carrierChanges: 40, address: true });
  // The link goes: the count rises, and NetworkManager takes the IPv4 address away. The IPv6
  // address that stays does not carry the route to this Mini.
  Object.assign(system.files, { '/sys/class/net/enp2s0/carrier': '0\n', '/sys/class/net/enp2s0/carrier_changes': '41\n' });
  system.addresses.enp2s0 = [v6('fe80::1'), v6('2001:db8::47')];
  assert.deepEqual(link.read(), { carrier: false, carrierChanges: 41, address: false });
  // The link is back and has only the address it gives itself, then a lease with another address
  // than before: the interface has an address again.
  Object.assign(system.files, { '/sys/class/net/enp2s0/carrier': '1\n', '/sys/class/net/enp2s0/carrier_changes': '42\n' });
  system.addresses.enp2s0 = [v6('fe80::1'), v4('169.254.7.7')];
  assert.deepEqual(link.read(), { carrier: true, carrierChanges: 42, address: false });
  system.addresses.enp2s0 = [v6('fe80::1'), v4('192.168.1.99')];
  assert.deepEqual(link.read(), { carrier: true, carrierChanges: 42, address: true });
  // The interface is gone from the system, as a USB adapter can be.
  delete system.addresses.enp2s0;
  system.files = {};
  assert.deepEqual(link.read(), { address: false });
  // A system without these files (macOS) reports the address alone.
  system.addresses.enp2s0 = [v4('192.168.1.102'), v6('2001:db8::47')];
  assert.deepEqual(link.read(), { address: true });
  // A Mini at an IPv6 address: the same interface, judged by its IPv6 addresses; https asks port 443.
  const link6 = await toolLink('https://[2001:db8::44]', options);
  assert.equal(link6.name, 'enp2s0');
  assert.deepEqual(asked.at(-1), ['[2001:db8::44]', 443]);
  system.addresses.enp2s0 = [v4('192.168.1.102'), v6('fe80::1')];
  assert.deepEqual(link6.read(), { address: false });
  // No route to the Mini, or an address of no interface: no state to read.
  assert.equal(await toolLink('http://10.9.9.9', options), null);
  assert.equal(await toolLink('http://192.168.1.100', { ...options, interfaces: () => ({ lo: [v4('127.0.0.1')] }) }), null);
});

test('freezeMs is the freeze time Chrome added in that second', () => {
  const rows = recording(10, { video: true });
  rows.forEach((row, second) => { row.browser.video.totalFreezesDuration = second >= 6 ? 0.31 : 0; });
  const { failures } = judge(rows, rules({ while: { video: true }, measure: 'freezeMs', max: 0 }));
  assert.deepEqual(failures.map(f => [f.t, f.value]), [[T0 + 6000, 310]]);
});

test('service video that falls back to HDMI capture fails, also at a full frame rate', () => {
  const rows = recording(10, { video: true, osVideo: true });
  rows.forEach((row, second) => { row.device.guestVideo = { source: second === 6 ? 'physical' : 'guest' }; });
  const criteria = rules(VIDEO, { while: { osVideo: true }, measure: 'serviceVideo', min: 1 });
  assert.deepEqual(judge(rows, criteria).failures.map(f => [f.measure, f.t]), [['serviceVideo', T0 + 6000]]);
});

test('videoPausedByUpload fails when the Mini reports no pause by the upload, and when the pause stays after it', () => {
  const criteria = rules(
    { measure: 'videoPausedByUpload', max: 0 },
    { while: { upload: true }, measure: 'videoPausedByUpload', min: 1 });
  const uploading = second => second >= 3 && second <= 8;
  const failuresWith = pausedBy => {
    const rows = recording(12, second => ({ video: true, upload: uploading(second) }));
    rows.forEach((row, second) => { row.device.videoPausedBy = pausedBy(second); });
    return judge(rows, criteria).failures.map(f => [f.t, f.value]);
  };
  assert.deepEqual(failuresWith(second => (uploading(second) ? ['upload'] : [])), []);
  // The pause of another source is not the upload's.
  assert.deepEqual(failuresWith(second => (uploading(second) ? ['ui'] : [])), [[T0 + 3000, 0]]);
  assert.deepEqual(failuresWith(second => (second >= 3 ? ['ui', 'upload'] : [])), [[T0 + 9000, 1]]);
});

test('videoQuantizer is the mean quantizer of the frames of that second, not of the stream', () => {
  const rows = recording(10, { video: true });
  // 30 frames a second: quantizer 20 for 8 seconds, then 40.
  rows.forEach((row, second) => { row.browser.video.qpSum = 30 * (20 * Math.min(second + 1, 8) + 40 * Math.max(0, second - 7)); });
  const { failures } = judge(rows, rules({ while: { video: true }, measure: 'videoQuantizer', max: 35 }));
  assert.deepEqual(failures.map(f => [f.t, f.value]), [[T0 + 8000, 40]]);
});

// Gives each answer of the Mini its encoder counters since boot. framesOf
// names the frames of a second: [frames that are not keyframes, the time of
// each in ms, keyframes, the time of each in ms, the hand-over of each frame
// and keyframe in ms].
function withEncoder(rows, framesOf) {
  const total = { pFrames: 0, pEncodeUs: 0, idrFrames: 0, idrEncodeUs: 0, handoffUs: 0 };
  rows.forEach((row, second) => {
    const [frames, ms, keyframes, keyframeMs, handoffMs = 0] = framesOf(second);
    total.pFrames += frames;
    total.pEncodeUs += frames * ms * 1000;
    total.idrFrames += keyframes;
    total.idrEncodeUs += keyframes * keyframeMs * 1000;
    total.handoffUs += (frames + keyframes) * handoffMs * 1000;
    if (row.device) {
      row.device.videoEncode = { ...total, pEncodeMaxUs: frames ? ms * 1000 : 0, idrEncodeMaxUs: keyframes ? keyframeMs * 1000 : 0,
        handoffMaxUs: frames + keyframes ? handoffMs * 1000 : 0 };
    }
  });
  return rows;
}

test('encodeMs and keyframeEncodeMs are the mean encode times of the frames since the Mini\'s answer before', () => {
  // 28 frames of 33 ms and 2 keyframes of 27 ms a second; second 5 has 30 frames of 35 ms and no keyframe.
  const rows = withEncoder(recording(10, { video: true }), second => (second === 5 ? [30, 35, 0, 0] : [28, 33, 2, 27]));
  // A second without a keyframe has no keyframe time, so it is not below the minimum.
  const limits = rules({ measure: 'encodeMs', max: 34 }, { measure: 'keyframeEncodeMs', min: 26, max: 28 });
  assert.deepEqual(judge(rows, limits).failures.map(f => [f.measure, f.t, f.value]), [['encodeMs', T0 + 5000, 35]]);
});

test('the frames encoded beside a missing answer of the Mini are in the mean of the next answer', () => {
  // The frames of second 5 take 37 ms and the Mini does not answer; those of second 6 take 33 ms.
  const rows = withEncoder(recording(10, { video: true }, { noDevice: [5] }), second => [30, second === 5 ? 37 : 33, 0, 0]);
  const { failures } = judge(rows, rules({ measure: 'encodeMs', max: 34 }));
  assert.deepEqual(failures.map(f => [f.t, f.value]), [[T0 + 6000, 35]]);
});

test('the frames encoded during a pause without an answer of the Mini are not counted after the pause', () => {
  // The Mini does not answer during the change, in which its frames take 60 ms; they take 33 ms after it.
  const rows = withEncoder(recording(10, { video: true }, { noDevice: [3, 4] }), second => [30, second === 3 || second === 4 ? 60 : 33, 0, 0]);
  const criteria = { everySecond: [{ measure: 'encodeMs', max: 34 }],
    changeTakesAtMost: [{ change: { source: true }, max: 5, pauses: ['encodeMs'] }] };
  assert.equal(judge([...rows, change('source', true, 2, 2)], criteria).result, 'pass');
  // Without the pause the same frames are in the answer after them.
  assert.deepEqual(judge(rows, { everySecond: criteria.everySecond }).failures.map(f => [f.t, f.value]), [[T0 + 5000, 51]]);
});

test('after a restart of the Mini, encodeMs is the mean of the frames since the restart', () => {
  // 5 frames of 33 ms before the restart. The restarted Mini has encoded 20 frames of 40 ms by its
  // first answer, more than before the restart, and 30 frames of 33 ms a second after it.
  const rows = recording(10, { video: true }, { restartAt: 5 });
  rows.forEach((row, second) => {
    row.device.videoEncode = second < 5 ? { pFrames: 5, pEncodeUs: 165_000 }
      : { pFrames: 20 + 30 * (second - 5), pEncodeUs: 800_000 + 990_000 * (second - 5) };
  });
  const { failures } = judge(rows, rules({ measure: 'encodeMs', max: 34 }));
  assert.deepEqual(failures.map(f => [f.t, f.value]), [[T0 + 5000, 40]]);
});

test('encodeMaxMs is the longest encode time the Mini reports, of a frame or of a keyframe', () => {
  const rows = recording(10, { video: true });
  rows.forEach((row, second) => {
    row.device.videoEncode = { pEncodeMaxUs: second === 4 ? 41_000 : 33_100, idrEncodeMaxUs: second === 7 ? 52_500 : 27_400 };
  });
  const { failures } = judge(rows, rules({ measure: 'encodeMaxMs', max: 34 }));
  assert.deepEqual(failures.map(f => [f.t, f.value]), [[T0 + 4000, 41], [T0 + 7000, 52.5]]);
});

test('encodeHandoffMs is the mean hand-over of the frames and keyframes since the Mini\'s answer before, and encodeHandoffMaxMs the longest', () => {
  // 28 frames and 2 keyframes a second wait 0.5 ms each for the encoder; those of second 5 wait 12 ms each.
  const rows = withEncoder(recording(10, { video: true }), second => [28, 33, 2, 27, second === 5 ? 12 : 0.5]);
  const limits = rules({ measure: ['encodeHandoffMs', 'encodeHandoffMaxMs'], max: 1 });
  assert.deepEqual(judge(rows, limits).failures.map(f => [f.measure, f.t, f.value]),
    [['encodeHandoffMs', T0 + 5000, 12], ['encodeHandoffMaxMs', T0 + 5000, 12]]);
});

test('a pause of a longest time lasts until the Mini no longer reports the frames of the change', () => {
  // A frame of 60 ms, or one that waited 60 ms, at the end of a change of 2 seconds stays in the Mini's answers for up to 4 seconds.
  const rows = slowFrom => recording(14, { video: true }).map((row, second) => {
    const slow = second >= slowFrom && second < slowFrom + 4;
    return { ...row, device: { ...row.device, videoEncode: { pEncodeMaxUs: slow ? 60_000 : 33_000, idrEncodeMaxUs: 27_000, handoffMaxUs: slow ? 60_000 : 500 } } };
  });
  for (const measure of ['encodeMaxMs', 'encodeHandoffMaxMs']) {
    const criteria = { everySecond: [{ measure, max: 34 }],
      changeTakesAtMost: [{ change: { source: true }, max: 5, pauses: [measure] }] };
    assert.equal(judge([...rows(4), change('source', true, 2, 2)], criteria).result, 'pass');
    // A frame that the Mini still reports one second later was encoded after the change.
    assert.deepEqual(judge([...rows(5), change('source', true, 2, 2)], criteria).failures.map(f => [f.t, f.value]), [[T0 + 8000, 60]]);
  }
});

test('the encoder measures have no value without the Mini\'s counters, and while the Mini encodes nothing', () => {
  // A value of 0 would be below these minimums.
  const limits = rules({ measure: ['encodeMs', 'keyframeEncodeMs', 'encodeMaxMs', 'encodeHandoffMs', 'encodeHandoffMaxMs'], min: 1 });
  // An old recording, or older firmware.
  assert.equal(judge(recording(10, { video: true }), limits).result, 'pass');
  // Service video: the sender encodes, so the Mini's counters stand still and its longest times are 0.
  const still = withEncoder(recording(10, { video: true, osVideo: true }), second => (second < 3 ? [28, 33, 2, 27, 2] : [0, 0, 0, 0]));
  assert.equal(judge(still, limits).result, 'pass');
});

// The frameGapMs of each answer of the page's frameTimes(), each in a sample of its own.
const frameGaps = answers => valueEachSecond(answers.map((frames, second) => ({ t: T0 + second * 1000, state: { video: true },
  device: { nowMs: 60_000 + second * 1000 }, browser: { connectionState: 'connected', video: { framesDecoded: second * 30 }, frames } })),
'frameGapMs');

test('frameGapMs is the longest time between two frames of the sample, from the frame before them on, and the time since the newest frame', () => {
  assert.deepEqual(frameGaps([
    null,
    { before: null, times: [1000, 1033, 1066, 1166, 1200], now: 1210 },
    { before: 1200, times: [1233, 1266], now: 1270 },
    // No frame: the time since the newest one.
    { before: 1266, times: [], now: 2270 },
    // The frame that ends a gap has the whole gap.
    { before: 1266, times: [3300], now: 3310 },
  ]), [null, 100, 33, 1004, 2034]);
});

test('the page keeps each frame\'s time until an answer that has it is in a sample, and a call that a later one overtook changes nothing', t => {
  // The page's side of frameTimes: a video element that notes the frame callback, and the page's clock.
  let onFrame = null;
  let clock = 0;
  const element = { requestVideoFrameCallback: callback => { onFrame = callback; } };
  globalThis.document = { querySelector: () => element };
  globalThis.window = {};
  t.mock.method(performance, 'now', () => clock);
  const present = ms => { clock = ms; onFrame(ms, { presentationTime: ms }); };
  const answers = [];
  const read = (...call) => {
    const answer = frameTimes(...call);
    answers.push(answer);
    return answer;
  };
  try {
    assert.equal(read(), null);
    for (const ms of [1000, 1033, 1066, 1166, 1200]) present(ms);
    clock = 1210;
    assert.deepEqual(read(), { before: null, times: [1000, 1033, 1066, 1166, 1200], now: 1210 });
    // Read: those frames are not in the next answer.
    for (const ms of [1233, 1266]) present(ms);
    clock = 1270;
    assert.deepEqual(read(), { before: 1200, times: [1233, 1266], now: 1270 });
    clock = 2270;
    assert.deepEqual(read(), { before: 1266, times: [], now: 2270 });
    // The recorder numbers its calls and names the newest one whose answer it has.
    present(3366);
    clock = 3370;
    read(1, 0);
    for (const ms of [3400, 3600, 3633]) present(ms);
    clock = 3640;
    assert.deepEqual(frameTimes(2, 1), { before: 3366, times: [3400, 3600, 3633], now: 3640 });
    // The answer of call 2 came too late for its sample: call 3 still has its frames.
    present(3666);
    clock = 3670;
    assert.deepEqual(read(3, 1), { before: 3366, times: [3400, 3600, 3633, 3666], now: 3670 });
    // Call 5 is slow, and call 6 overtakes it and is in a sample. When call 5 ends, it changes nothing.
    present(3700);
    clock = 3710;
    read(4, 3);
    for (const ms of [3733, 3933]) present(ms);
    clock = 3940;
    read(6, 4);
    present(4366);
    clock = 4370;
    assert.equal(frameTimes(5, 4), null);
    present(4400);
    clock = 4410;
    assert.deepEqual(read(7, 6), { before: 3933, times: [4366, 4400], now: 4410 });
    // The gaps of these answers, each in its sample: the 200 ms of the late call 2 are in call 3's.
    assert.deepEqual(frameGaps(answers), [null, 100, 33, 1004, 2100, 200, 34, 200, 433]);
    // A new video element, as after the page was loaded again, has no frame yet.
    globalThis.document = { querySelector: () => ({ requestVideoFrameCallback: () => {} }) };
    assert.equal(frameTimes(), null);
  } finally {
    delete globalThis.document;
    delete globalThis.window;
  }
});

// A recording in which the Mini's counters have these values in each second;
// a missing value is the one before. restartAt: the Mini's clock starts again.
function counted(seconds, counters, { restartAt = null, source = () => 'physical' } = {}) {
  const rows = recording(seconds, { video: true }, { restartAt });
  const now = { accessUnitsOversize: 0, accessUnitsNoSlot: 0, accessUnitsStalled: 0, framesDropped: 0 };
  rows.forEach((row, second) => {
    Object.assign(now, counters[second]);
    const { framesDropped, ...transport } = now;
    Object.assign(row.device, transport, { guestVideo: { source: source(second), framesDropped } });
  });
  return rows;
}
const values = (rows, measure) => judge(rows, rules({ measure, max: 0 })).failures.map(f => [f.t - T0, f.value]);
// The value of a measure in each second, null without one.
const valueEachSecond = (rows, measure) => secondsNotNormal(rows, rules({ measure, max: -1 }))
  .map(second => second.notNormal.find(entry => entry.measure === measure)?.value ?? null);

test('framesTooBig is what the Mini\'s count of oversize frames grew by, also beside a missing answer', () => {
  assert.deepEqual(values(counted(10, { 4: { accessUnitsOversize: 2 }, 7: { accessUnitsOversize: 3 } }), 'framesTooBig'),
    [[4000, 2], [7000, 1]]);
  const rows = counted(10, { 4: { accessUnitsOversize: 2 } });
  rows[4].device = null;
  assert.deepEqual(values(rows, 'framesTooBig'), [[5000, 2]]);
});

test('a Mini that restarts counts from zero: its lower count is no growth, and a count after the restart is', () => {
  // 5 frames before the restart; the Mini is back in second 6 with its counter at 0, then at 1.
  const before = { 2: { accessUnitsOversize: 5 }, 6: { accessUnitsOversize: 0 } };
  assert.deepEqual(values(counted(10, before, { restartAt: 6 }), 'framesTooBig'), [[2000, 5]]);
  assert.deepEqual(values(counted(10, { ...before, 8: { accessUnitsOversize: 1 } }, { restartAt: 6 }), 'framesTooBig'),
    [[2000, 5], [8000, 1]]);
  // The first answer after the restart has more than the last one before it: all of it is new.
  assert.deepEqual(values(counted(10, { 2: { accessUnitsOversize: 5 }, 6: { accessUnitsOversize: 7 } }, { restartAt: 6 }), 'framesTooBig'),
    [[2000, 5], [6000, 7]]);
});

test('framesSkipped counts frames without a slot with HDMI capture, dropped frames with service video, and stalled frames with both', () => {
  // HDMI capture until second 9, service video from second 10 on.
  const rows = counted(20, {
    3: { accessUnitsNoSlot: 4 },
    5: { accessUnitsStalled: 1 },
    // A service video frame that waits for a slot asks for it many times.
    10: { accessUnitsNoSlot: 204 },
    12: { accessUnitsNoSlot: 450, framesDropped: 2 },
    14: { accessUnitsNoSlot: 451, accessUnitsStalled: 2 },
  }, { source: second => (second < 10 ? 'physical' : 'guest') });
  assert.deepEqual(values(rows, 'framesSkipped'), [[3000, 4], [5000, 1], [12000, 2], [14000, 1]]);
  // A firmware without OS Services reports no service video: HDMI capture.
  const plain = counted(6, { 3: { accessUnitsNoSlot: 4 } });
  plain.forEach(row => { row.device.guestVideo = null; });
  assert.deepEqual(values(plain, 'framesSkipped'), [[3000, 4]]);
  // The answer of second 4 is missing: the 6 frames of the answer of second 5 are those of two seconds.
  const gap = counted(8, { 5: { accessUnitsNoSlot: 6 } });
  gap[4].device = null;
  assert.deepEqual(values(gap, 'framesSkipped'), [[5000, 3]]);
});

// A recording in which the Mini's full stats have these values in each
// second, as the recorder keeps them (memoryCounters); a missing value is
// the one before, and null is a second without that answer. restartAt: the
// Mini's clock starts again.
const FULL_STATS = {
  memoryBudget: { controlAllocationFailures: 0, jsonAllocationFailures: 0, lvglAllocationFailures: 0, videoRebuildFailures: 0,
    videoRebuildBeginDenials: 0, admissionDenials: 0, tlsAdmissions: [] },
  peerMemoryArena: { allocationFailures: 0, beginFailures: 0 },
  usbHid: { uploadWrite: { errors: 0, stagingAllocationFailures: 0 } },
  ethernetRx: { pbufAllocationFailures: 0 },
  internalHeapMinimum: 90_000, internalDmaHeapMinimum: 70_000, psramMinimum: 5_000_000,
};
function withCounters(seconds, answers, { restartAt = null, state = { video: true } } = {}) {
  const rows = recording(seconds, state, { restartAt });
  let now = FULL_STATS;
  rows.forEach((row, second) => {
    if (answers[second] === null) return void (row.counters = null);
    now = structuredClone(now);
    for (const [place, value] of Object.entries(answers[second] ?? {})) {
      const keys = place.split('.');
      keys.slice(0, -1).reduce((object, key) => object[key], now)[keys.at(-1)] = value;
    }
    row.counters = memoryCounters(now, { nowMs: row.device.nowMs });
  });
  return rows;
}

test('the recorder keeps the Mini\'s counts of failures, refusals per owner and least free memory of the full stats, and nothing of the short answer', () => {
  const answer = structuredClone(FULL_STATS);
  answer.memoryBudget.jsonAllocationFailures = 2;
  answer.memoryBudget.tlsAdmissions = [
    { owner: 'https-media-range', admitted: 950, denied: 2, lastInternalHeadroom: 31_000, lastDmaHeadroom: 4000,
      minInternalHeadroom: 30_000, minDmaHeadroom: -853 },
    { owner: 'ota-download', admitted: 1, denied: 0, lastInternalHeadroom: 50_000, lastDmaHeadroom: 20_000,
      minInternalHeadroom: 50_000, minDmaHeadroom: 20_000, minInternalLargest: 47_104, minDmaLargest: 47_104,
      reservedInternal: 8192, reservedDma: 8192,
      heldInternalMin: 118_000, heldDmaMin: 79_000, heldInternalFallMax: 4900, heldDmaFallMax: 4900, heldOverruns: 0,
      heldUnderFloor: 0 }];
  answer.internalHeapFree = 120_000;
  answer.memoryBudget.admissionDenials = 7;
  answer.usbHid.uploadWrite.errors = 3;
  delete answer.ethernetRx;
  const kept = memoryCounters(answer, { nowMs: 5000, answerMs: 180 });
  assert.equal(kept.allocationFailures['memoryBudget.jsonAllocationFailures'], 2);
  // An answer without the Ethernet receive counters: the counter is left out, not 0.
  assert.deepEqual(Object.keys(kept.allocationFailures), ALLOCATION_FAILURES.filter(place => !place.startsWith('ethernetRx')));
  // The first owner is from firmware without the values of a hold.
  const noHold = { minInternalLargest: undefined, minDmaLargest: undefined, reservedInternal: undefined, reservedDma: undefined,
    heldInternalMin: undefined, heldDmaMin: undefined, heldInternalFallMax: undefined, heldDmaFallMax: undefined,
    heldOverruns: undefined, heldUnderFloor: undefined };
  assert.deepEqual(kept.tlsAdmissions, {
    'https-media-range': { admitted: 950, denied: 2, minInternalHeadroom: 30_000, minDmaHeadroom: -853,
      lastInternalHeadroom: 31_000, lastDmaHeadroom: 4000, ...noHold },
    'ota-download': { admitted: 1, denied: 0, minInternalHeadroom: 50_000, minDmaHeadroom: 20_000,
      lastInternalHeadroom: 50_000, lastDmaHeadroom: 20_000, minInternalLargest: 47_104, minDmaLargest: 47_104,
      reservedInternal: 8192, reservedDma: 8192,
      heldInternalMin: 118_000, heldDmaMin: 79_000, heldInternalFallMax: 4900, heldDmaFallMax: 4900, heldOverruns: 0,
      heldUnderFloor: 0 } });
  assert.equal(kept.freeInternal, 120_000);
  assert.deepEqual([kept.admissionDenials, kept.uploadWriteErrors, kept.nowMs, kept.answerMs], [7, 3, 5000, 180]);
  assert.deepEqual([kept.freeInternalLowWater, kept.freeDmaLowWater, kept.freePsramLowWater], [90_000, 70_000, 5_000_000]);
  assert.equal(memoryCounters({ nowMs: 5, internalDmaHeapMinimum: 70_000 }), null);
  assert.equal(memoryCounters(undefined), null);
});

test('the row with the Mini\'s tasks has the list of the detailed stats as it came, and an error without it', async () => {
  const tasks = [{ name: 'mqtt_task', coreId: -1, configuredBytes: 6144, highWatermarkBytes: 480, peakUsedBytes: 5664, stackMemory: 'psram' }];
  const details = { cached: false, available: true, scanUs: 900, maxMaskedUs: 40, truncated: false };
  const asked = [];
  const row = await taskStacksRow('http://192.0.2.1', 'end of the list', async address => {
    asked.push(String(address));
    return { nowMs: 5, taskStackDetails: details, taskStacks: tasks };
  });
  assert.deepEqual(asked, ['http://192.0.2.1/webrtc/stats?details=1']);
  assert.equal(typeof row.taskStacks.answerMs, 'number');
  assert.equal(typeof row.taskStacks.askedAt, 'number');
  assert.deepEqual({ ...row.taskStacks, askedAt: 0, answerMs: 0 }, { at: 'end of the list', askedAt: 0, answerMs: 0, details, tasks });
  // A sample has `device`; this row must not be taken for one.
  assert.equal('device' in row, false);
  // An answer of a firmware without the list, and no answer.
  const failed = async answer => { const { taskStacks: { askedAt, ...rest } } = await taskStacksRow('http://192.0.2.1', 'before the update', answer); return rest; };
  assert.deepEqual(await failed(async () => ({ nowMs: 5 })), { at: 'before the update', error: 'the answer has no taskStacks' });
  assert.deepEqual(await failed(async () => { throw new Error('fetch failed'); }), { at: 'before the update', error: 'fetch failed' });
});

test('the recorder asks for the full stats every 5 s, and half as often after each answer that was slow or did not come', () => {
  assert.equal(fullStatsPause(5000, 80), 5000);
  // 600 ms, then no answer, then no answer: 10 s, 20 s, 40 s, and no longer.
  assert.deepEqual([600, null, null, null].reduce((pauses, answerMs) => [...pauses, fullStatsPause(pauses.at(-1), answerMs)], [5000]),
    [5000, 10_000, 20_000, 40_000, 40_000]);
  // A quick answer brings the pause back at once.
  assert.equal(fullStatsPause(40_000, 120), 5000);
  // In a state that restarts the Mini the pause is the shortest, also after slow answers.
  assert.deepEqual([pauseNow(40_000, false), pauseNow(40_000, true)], [40_000, 5000]);
  // Five samples of 999 ms after the one that asked, the sample asks: the answers are 5 s apart, not 6 s.
  assert.deepEqual([4 * 999, 5 * 999, 5 * 1001].map(sinceMs => fullStatsDue(sinceMs, 5000)), [false, true, true]);
  assert.deepEqual([9 * 999, 10 * 999].map(sinceMs => fullStatsDue(sinceMs, 10_000)), [false, true]);
});

// A clock that only the test moves, for startRecorder. sleep(ms) ends when
// the time has gone on by ms; of two sleeps that end at the same time the
// one that began first ends first. Before each step of the time, every
// promise runs that can run without it, so a run has one order.
function testClock() {
  let now = T0;
  const sleeps = [];
  const settle = () => new Promise(resolve => setImmediate(resolve));
  const step = async until => {
    await settle();
    const next = sleeps.reduce((first, sleep) => (!first || sleep.at < first.at ? sleep : first), null);
    if (!next || next.at > until) return false;
    sleeps.splice(sleeps.indexOf(next), 1);
    now = next.at;
    next.end();
    return true;
  };
  return {
    now: () => now,
    sleep: ms => new Promise(end => sleeps.push({ at: now + Math.max(0, ms), end })),
    // The time goes on by ms.
    advance: async ms => {
      const end = now + ms;
      while (await step(end));
      now = end;
    },
  };
}

test('a switch\'s sample that comes after its time is in no row, and its events go into the switch\'s next row before that row\'s own', async () => {
  const clock = testClock();
  // The terminal's second sample takes 1.5 s; each sample has one break, with the number of its call.
  let calls = 0;
  const osTerminal = { sample: async () => {
    const call = calls++;
    if (call === 1) await clock.sleep(1500);
    return { lines: call, breaks: [[call]] };
  } };
  const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-')), 'late.jsonl');
  const recorder = startRecorder(file, { page: { evaluate: async () => null }, url: 'http://mini.test', switches: { osTerminal },
    state: { osTerminal: true }, clock, get: async () => ({ nowMs: clock.now() - T0 }) });
  await clock.advance(3500);
  const stopped = recorder.stop();
  await clock.advance(7000);
  await stopped;
  const rows = fs.readFileSync(file, 'utf8').split('\n').filter(Boolean).map(line => JSON.parse(line));
  // The sample of 1 s has no answer in its 900 ms. Its answer comes at 2.5 s, after the row of 2 s.
  assert.deepEqual(rows.map(row => row.loads.osTerminal?.breaks ?? null), [[[0]], null, [[2]], [[1], [3]], [[4]]]);
  // The other fields are those of the row's own sample.
  assert.equal(rows[3].loads.osTerminal.lines, 3);
  // The late sample of a switch that is off by its next row is in no row: the sample of 1 s again takes
  // 1.5 s, and the terminal is off from 2 s on.
  calls = 0;
  const state = { osTerminal: true };
  const offFile = file.replace('late', 'off');
  const off = startRecorder(offFile, { page: { evaluate: async () => null }, url: 'http://mini.test', switches: { osTerminal }, state, clock,
    get: async () => ({ nowMs: clock.now() - T0 }) });
  await clock.advance(1500);
  state.osTerminal = false;
  await clock.advance(1000);
  state.osTerminal = true;
  await clock.advance(1000);
  const ended = off.stop();
  await clock.advance(7000);
  await ended;
  const offRows = fs.readFileSync(offFile, 'utf8').split('\n').filter(Boolean).map(line => JSON.parse(line));
  assert.deepEqual(offRows.map(row => row.loads.osTerminal?.breaks ?? null), [[[0]], null, null, [[2]], [[3]]]);
  // The same holds for a switch that stays on while another variable
  // changes: the late sample's events are of the state before, whose rules
  // do not judge the next row. The sample of 1 s again takes 1.5 s, and the
  // update begins at 1.5 s.
  calls = 0;
  const changing = { osTerminal: true, ota: false };
  const changedFile = file.replace('late', 'changed');
  const changed = startRecorder(changedFile, { page: { evaluate: async () => null }, url: 'http://mini.test', switches: { osTerminal },
    state: changing, clock, get: async () => ({ nowMs: clock.now() - T0 }) });
  await clock.advance(1500);
  changing.ota = true;
  await clock.advance(2000);
  const changeEnded = changed.stop();
  await clock.advance(7000);
  await changeEnded;
  const changedRows = fs.readFileSync(changedFile, 'utf8').split('\n').filter(Boolean).map(line => JSON.parse(line));
  assert.deepEqual(changedRows.map(row => [row.state.ota, row.loads.osTerminal?.breaks ?? null]),
    [[false, [[0]]], [false, null], [true, [[2]]], [true, [[3]]], [true, [[4]]]]);
});

test('the recorder does not ask for the full stats in a state that the run names, except in its last sample, which waits for the slow answer', async () => {
  const clock = testClock();
  // A Mini that answers after the time that answerMs(full, asked) gives; asked
  // counts its requests. Its clock is at 9 s when it is made.
  const miniOf = (answerMs, fullAnswer) => {
    const asked = { short: 0, full: 0 };
    const made = clock.now();
    const get = async url => {
      const full = !String(url).includes('bitrate=1');
      const count = asked[full ? 'full' : 'short']++;
      await clock.sleep(answerMs(full, count));
      return full ? fullAnswer(count) : { nowMs: 9000 + clock.now() - made };
    };
    return { asked, get };
  };
  // A Mini that needs 1.5 s for the full stats and answers the short ones after shortMs.
  let shortMs = 0;
  const slowFull = () => miniOf(full => (full ? 1500 : shortMs), () => ({ memoryBudget: { videoRebuildFailures: 2 } }));
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const start = (name, mini, options) => {
    const file = path.join(directory, `${name}.jsonl`);
    const recorder = startRecorder(file, { page: { evaluate: async () => null }, url: 'http://mini.test', switches: {}, clock, get: mini.get, ...options });
    // The last sample begins within 1 s and takes at most 0.9 s and the 5 s
    // of its full stats; after that only the file is waited for.
    const stop = async () => {
      const stopped = recorder.stop();
      await clock.advance(7000);
      await stopped;
    };
    return { stop, rows: () => fs.readFileSync(file, 'utf8').split('\n').filter(Boolean).map(line => JSON.parse(line)) };
  };
  const record = async (name, withoutFullStats) => {
    const mini = slowFull();
    const recorder = start(name, mini, { state: { video: true, tfMountCdrom: true }, withoutFullStats });
    await clock.advance(1500);
    await recorder.stop();
    const rows = recorder.rows();
    return { asked: mini.asked, counters: rows.map(row => row.counters?.videoRebuildFailures ?? null), answered: rows.map(row => row.device !== null) };
  };
  // Two samples and the last one: only the last asks, and has the answer.
  const quiet = await record('quiet', state => state.tfMountCdrom);
  assert.deepEqual([quiet.asked, quiet.counters, quiet.answered], [{ short: 3, full: 1 }, [null, null, 2], [true, true, true]]);
  // Without such a state the first sample asks and gets no answer in its
  // 900 ms; the last sample asks again and waits.
  const usual = await record('usual', () => false);
  assert.deepEqual([usual.asked, usual.counters], [{ short: 3, full: 2 }, [null, null, 2]]);
  // A Mini whose short answer takes longer than a sample has: the last
  // sample asks for the full stats all the same, and has them.
  shortMs = 2000;
  const silent = await record('silent', state => state.tfMountCdrom);
  shortMs = 0;
  assert.deepEqual([silent.asked.full, silent.counters.at(-1), silent.answered.at(-1)], [1, 2, false]);
  // A state that restarts the Mini begins: its first
  // sample asks once, without a wait and with no other request beside it, and the answer is in the sample
  // that follows its arrival, 1.5 s later, with the clock of the asking sample.
  const restarts = { state: { video: true, ota: true }, beforeRestart: state => state.ota };
  const mini = slowFull();
  const recorder = start('restart', mini, restarts);
  await clock.advance(2500);
  const before = { ...mini.asked };
  await recorder.stop();
  assert.equal(before.full, 1);
  assert.deepEqual(recorder.rows().slice(0, 3).map(row => [row.counters?.videoRebuildFailures ?? null, row.counters?.nowMs ?? null]), [[null, null], [null, null], [2, 9000]]);
  // The answer from before the restart takes 4.6 s, and the request of
  // the sample at 5 s is answered at once with a higher count: the rows
  // have the older count first and no count under an earlier one.
  const counts = miniOf((full, asked) => (full && asked === 0 ? 4600 : 0), asked => ({ memoryBudget: { videoRebuildFailures: asked === 0 ? 2 : 3 } }));
  const ordered = start('order', counts, restarts);
  await clock.advance(6500);
  const asks = counts.asked.full;
  await ordered.stop();
  // The sample at 5 s did ask: its own count of 3 is not in its row.
  assert.equal(asks, 2);
  assert.deepEqual(ordered.rows().map(row => row.counters?.videoRebuildFailures ?? null), [null, null, null, null, null, 2, null, 3]);
  // The Mini's short answer to the first sample of such a state does not
  // come in the sample's time: that sample does not ask, and the second does.
  shortMs = 2000;
  const lateMini = slowFull();
  const late = start('late', lateMini, restarts);
  await clock.advance(500);
  shortMs = 0;
  await clock.advance(2300);
  const lateAsks = lateMini.asked.full;
  await late.stop();
  const lateRows = late.rows();
  assert.equal(lateAsks, 1);
  assert.deepEqual([lateRows[0].device, lateRows[0].counters], [null, null]);
  assert.equal(lateRows.find(row => row.counters)?.counters.videoRebuildFailures, 2);
});

test('allocationFailures is the growth of all the Mini\'s counters of failed allocations together, also beside a missing answer', () => {
  const rows = withCounters(10, {
    3: { 'memoryBudget.jsonAllocationFailures': 1, 'peerMemoryArena.beginFailures': 2 },
    5: null,
    6: { 'usbHid.uploadWrite.stagingAllocationFailures': 4 },
  });
  // The failures beside the missing answer of second 5 are in the answer of second 6.
  assert.deepEqual(values(rows, 'allocationFailures'), [[3000, 3], [6000, 4]]);
  // Counts that the Mini had before the recording are not this recording's.
  const old = withCounters(6, { 0: { 'memoryBudget.controlAllocationFailures': 9 } });
  assert.deepEqual(values(old, 'allocationFailures'), []);
  // A recording without the full stats, as from a tool before them, has no value.
  const without = recording(6, { video: true });
  assert.equal(judge(without, rules({ measure: 'allocationFailures', max: 0 })).result, 'pass');
});

test('videoRebuildFailures and uploadWriteErrors are each the growth of their own counter, and a restarted Mini counts from zero', () => {
  const rows = withCounters(12, {
    2: { 'memoryBudget.videoRebuildFailures': 1 },
    4: { 'usbHid.uploadWrite.errors': 3 },
    // The Mini is back in second 7 with its counters at 0; then one write fails.
    7: { 'memoryBudget.videoRebuildFailures': 0, 'usbHid.uploadWrite.errors': 0 },
    9: { 'usbHid.uploadWrite.errors': 1 },
  }, { restartAt: 7 });
  assert.deepEqual(values(rows, 'videoRebuildFailures'), [[2000, 1]]);
  assert.deepEqual(values(rows, 'uploadWriteErrors'), [[4000, 3], [9000, 1]]);
  // A refused begin of a rebuild is a wait, not a failed rebuild.
  assert.deepEqual(values(withCounters(6, { 3: { 'memoryBudget.videoRebuildBeginDenials': 40 } }), 'videoRebuildFailures'), []);
});

test('imageReadsRefused is the growth of the refused requests of the image\'s reads, in every state, and an owner that the Mini does not list yet counts 0', () => {
  const owners = (...list) => list.map(([owner, admitted, denied]) => ({ owner, admitted, denied }));
  const rows = withCounters(12, {
    // The Mini refused 3 reads before the recording, and refuses requests of another owner in it.
    0: { 'memoryBudget.tlsAdmissions': owners(['https-media-range', 900, 3], ['osvc-update', 2, 0]) },
    4: { 'memoryBudget.tlsAdmissions': owners(['https-media-range', 950, 4], ['osvc-update', 2, 5]) },
    6: null,
    7: { 'memoryBudget.tlsAdmissions': owners(['https-media-range', 990, 6], ['osvc-update', 2, 5]) },
  });
  assert.deepEqual(values(rows, 'imageReadsRefused'), [[4000, 1], [7000, 2]]);
  // After a restart the Mini lists the owner only from its first read on: a refusal in that answer is one.
  const restarted = withCounters(10, {
    0: { 'memoryBudget.tlsAdmissions': owners(['https-media-range', 900, 3]) },
    5: { 'memoryBudget.tlsAdmissions': [] },
    7: { 'memoryBudget.tlsAdmissions': owners(['https-media-range', 10, 1]) },
  }, { restartAt: 5 });
  assert.deepEqual(values(restarted, 'imageReadsRefused'), [[7000, 1]]);
  // A read that had to open the image's connection first, and was refused that, is a refused read as well.
  const connecting = withCounters(10, {
    0: { 'memoryBudget.tlsAdmissions': owners(['https-media-range', 900, 0], ['https-media-connect', 1, 0], ['ota-download', 1, 0]) },
    4: { 'memoryBudget.tlsAdmissions': owners(['https-media-range', 950, 0], ['https-media-connect', 1, 2], ['ota-download', 2, 0]) },
    8: { 'memoryBudget.tlsAdmissions': owners(['https-media-range', 950, 1], ['https-media-connect', 1, 5], ['ota-download', 2, 0]) },
  });
  assert.deepEqual(values(connecting, 'imageReadsRefused'), [[4000, 2], [8000, 4]]);
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    assert.deepEqual(judge(rows, criteria).failures.filter(f => f.measure === 'imageReadsRefused').map(f => [f.measure, f.t - T0, f.value]),
      [['imageReadsRefused', 4000, 1], ['imageReadsRefused', 7000, 2]], board);
  }
});

test('senderUpdatesRefused is the growth of the refused requests of the sender\'s updates, fails on both boards, and an owner that the Mini does not list yet counts 0', () => {
  const owners = (...list) => list.map(([owner, admitted, denied]) => ({ owner, admitted, denied }));
  const rows = withCounters(12, {
    // The Mini refused 2 before the recording, and refuses requests of an image's reads in it.
    0: { 'memoryBudget.tlsAdmissions': owners(['osvc-update', 4, 2], ['https-media-range', 900, 0]) },
    4: { 'memoryBudget.tlsAdmissions': owners(['osvc-update', 5, 3], ['https-media-range', 950, 4]) },
    6: null,
    7: { 'memoryBudget.tlsAdmissions': owners(['osvc-update', 5, 5], ['https-media-range', 990, 4]) },
  });
  assert.deepEqual(values(rows, 'senderUpdatesRefused'), [[4000, 1], [7000, 2]]);
  // After a restart the Mini lists the owner only from its first connection on: a refusal in that answer is one.
  const restarted = withCounters(10, {
    0: { 'memoryBudget.tlsAdmissions': owners(['osvc-update', 4, 2]) },
    5: { 'memoryBudget.tlsAdmissions': [] },
    7: { 'memoryBudget.tlsAdmissions': owners(['osvc-update', 0, 1]) },
  }, { restartAt: 5 });
  assert.deepEqual(values(restarted, 'senderUpdatesRefused'), [[7000, 1]]);
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    assert.deepEqual(judge(rows, criteria).failures.filter(f => f.measure === 'senderUpdatesRefused').map(f => [f.t - T0, f.value]),
      [[4000, 1], [7000, 2]], board);
  }
});

test('tlsHeldOverruns is the growth of the overrun holds over all owners, fails on both boards, and has no value for firmware without the count', () => {
  const owners = (...list) => list.map(([owner, heldOverruns]) => ({ owner, admitted: 5, denied: 0, heldOverruns, heldUnderFloor: 7 }));
  const rows = withCounters(12, {
    // One overrun before the recording, which is not the recording's.
    0: { 'memoryBudget.tlsAdmissions': owners(['ota-download', 1], ['https-media-range', 0]) },
    4: { 'memoryBudget.tlsAdmissions': owners(['ota-download', 1], ['https-media-range', 2]) },
    6: null,
    // A new owner with an overrun, and one more of the first.
    7: { 'memoryBudget.tlsAdmissions': owners(['ota-download', 2], ['https-media-range', 2], ['webrtc-dtls', 1]) },
  });
  assert.deepEqual(values(rows, 'tlsHeldOverruns'), [[4000, 2], [7000, 2]]);
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    assert.deepEqual(judge(rows, criteria).failures.map(f => [f.measure, f.t - T0, f.value]),
      [['tlsHeldOverruns', 4000, 2], ['tlsHeldOverruns', 7000, 2]], board);
    // Holds under an admission floor are recorded and fail nothing: the room between the floors is for them.
    const underFloor = withCounters(8, {
      0: { 'memoryBudget.tlsAdmissions': [{ owner: 'osvc-update', admitted: 4, denied: 0, heldOverruns: 0, heldUnderFloor: 0 }] },
      4: { 'memoryBudget.tlsAdmissions': [{ owner: 'osvc-update', admitted: 5, denied: 0, heldOverruns: 0, heldUnderFloor: 1 }] },
    });
    assert.deepEqual(judge(underFloor, criteria).failures, [], board);
  }
  // A restarted Mini counts from zero: its first overrun is one.
  const restarted = withCounters(10, {
    0: { 'memoryBudget.tlsAdmissions': owners(['ota-download', 3]) },
    5: { 'memoryBudget.tlsAdmissions': [] },
    7: { 'memoryBudget.tlsAdmissions': owners(['mqtt-reconnect', 1]) },
  }, { restartAt: 5 });
  assert.deepEqual(values(restarted, 'tlsHeldOverruns'), [[7000, 1]]);
  // An owner that the Mini only refused has no count, and the others' counts are judged.
  const refused = withCounters(8, {
    0: { 'memoryBudget.tlsAdmissions': owners(['ota-download', 0]) },
    4: { 'memoryBudget.tlsAdmissions': [...owners(['ota-download', 1]), { owner: 'public-ip', admitted: 0, denied: 1 }] },
  });
  assert.deepEqual(values(refused, 'tlsHeldOverruns'), [[4000, 1]]);
  // Firmware whose owners have no such count: no value, and no failure.
  const before = withCounters(8, {
    0: { 'memoryBudget.tlsAdmissions': [{ owner: 'ota-download', admitted: 1, denied: 0 }] },
    4: { 'memoryBudget.tlsAdmissions': [{ owner: 'ota-download', admitted: 2, denied: 0 }] },
  });
  assert.deepEqual(values(before, 'tlsHeldOverruns'), []);
});

test('on both boards a failure that the Mini counts fails in its second in a state that names no variable of it, and least free memory under a hard floor fails', () => {
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const failed = answers => judge(withCounters(8, answers), criteria).failures.map(f => [f.measure, f.t - T0, f.value]);
    assert.deepEqual(failed({}), [], board);
    assert.deepEqual(failed({ 4: { 'memoryBudget.videoRebuildFailures': 1 } }), [['videoRebuildFailures', 4000, 1]], board);
    assert.deepEqual(failed({ 4: { 'usbHid.uploadWrite.errors': 1 } }), [['uploadWriteErrors', 4000, 1]], board);
    assert.deepEqual(failed({ 4: { 'peerMemoryArena.allocationFailures': 1 } }), [['allocationFailures', 4000, 1]], board);
    // The floors are 48 KiB and 2 MiB; the mark stays where it fell, and the breach is reported once.
    assert.deepEqual(failed({ 4: { internalDmaHeapMinimum: 49_152 } }), [], board);
    assert.deepEqual(failed({ 4: { internalDmaHeapMinimum: 49_151 } }), [['freeDmaLowWater', 4000, 49_151]], board);
    assert.deepEqual(failed({ 4: { internalHeapMinimum: 40_000 } }), [['freeInternalLowWater', 4000, 40_000]], board);
    assert.deepEqual(failed({ 4: { psramMinimum: 2_097_151 } }), [['freePsramLowWater', 4000, 2_097_151]], board);
  }
});

test('tlsReserveMin is the least reserve of the update checks since the full answer before, also of the last check of a state, and has no value beside an HTTPS image', () => {
  const check = (admitted, lastInternalHeadroom, lastDmaHeadroom, least = {}) => [
    // Another owner's connection left less: it is not an update check.
    { owner: 'https-media-range', admitted: 400, denied: 0, lastInternalHeadroom: 12_000, lastDmaHeadroom: 6000 },
    { owner: 'ota-download', admitted, denied: 0, lastInternalHeadroom, lastDmaHeadroom, ...least }];
  const failed = (answers, state = { video: true, updateCheck: true }, board = 'ethernet') =>
    judge(withCounters(12, answers, { state }), readJsonc(path.join(here, '..', board, 'criteria.jsonc')))
      .failures.map(f => [f.measure, f.t - T0, f.value]);
  for (const board of ['ethernet', 'wifi']) {
    const both = answers => failed(answers, undefined, board);
    // The Mini has made no check yet, and then one that left 27 kB and 21 kB.
    assert.deepEqual(both({ 0: { 'memoryBudget.tlsAdmissions': check(0, 0, 0).slice(0, 1) },
      4: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) } }), [], board);
    // The DMA-capable RAM is the smaller one, and 16384 is the limit.
    assert.deepEqual(both({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) },
      4: { 'memoryBudget.tlsAdmissions': check(2, 22_000, 16_384) } }), [], board);
    assert.deepEqual(both({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) },
      4: { 'memoryBudget.tlsAdmissions': check(2, 22_000, 16_383) } }), [['tlsReserveMin', 4000, 16_383]], board);
    assert.deepEqual(both({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) },
      4: { 'memoryBudget.tlsAdmissions': check(2, 9000, 21_000) } }), [['tlsReserveMin', 4000, 9000]], board);
  }
  // A check from before the recording is not this recording's, however little it left.
  assert.deepEqual(failed({ 0: { 'memoryBudget.tlsAdmissions': check(5, 9000, 9000) } }), []);
  // The check of second 4 left enough; the answers after it have no new check, so the breach of
  // second 2 is over and the check of second 9 is a new one.
  assert.deepEqual(failed({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) },
    2: { 'memoryBudget.tlsAdmissions': check(2, 9000, 21_000) },
    4: { 'memoryBudget.tlsAdmissions': check(3, 27_000, 21_000) },
    9: { 'memoryBudget.tlsAdmissions': check(4, 8000, 21_000) } }), [['tlsReserveMin', 2000, 9000], ['tlsReserveMin', 9000, 8000]]);
  // Beside a mounted HTTPS image a check leaves less by design, also in the first answer after the image.
  assert.deepEqual(failed({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) },
    4: { 'memoryBudget.tlsAdmissions': check(2, 16_000, 9700) } }, { video: true, httpsMountDisk: true, updateCheck: true }), []);
  assert.deepEqual(failed({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) },
    6: { 'memoryBudget.tlsAdmissions': check(2, 16_000, 9700) } },
  second => (second < 5 ? { video: true, httpsMountDisk: true, updateCheck: true } : { video: true })), []);
  // The last check of the state is in the first full answer after it, which is judged.
  const sparse = (answers, state) => {
    const rows = withCounters(12, answers, { state });
    rows.forEach((row, second) => { if (!(second in answers)) row.counters = null; });
    return judge(rows, readJsonc(path.join(here, '..', 'ethernet', 'criteria.jsonc'))).failures.map(f => [f.measure, f.t - T0, f.value]);
  };
  const thenVideo = second => (second < 5 ? { video: true, updateCheck: true } : { video: true });
  assert.deepEqual(sparse({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) },
    7: { 'memoryBudget.tlsAdmissions': check(2, 9000, 21_000) } }, thenVideo), [['tlsReserveMin', 7000, 9000]]);
  // A connection of that owner in a state without the checks, as of an update, is not judged.
  assert.deepEqual(sparse({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000) },
    7: { 'memoryBudget.tlsAdmissions': check(2, 9000, 21_000) } }, { video: true }), []);
  // Two checks between two full answers: the answer does not show the first, so a least value since the
  // Mini's start that is lower than in the answer before counts, here of that check.
  const least = (minInternalHeadroom, minDmaHeadroom) => ({ minInternalHeadroom, minDmaHeadroom });
  const twoChecks = (before, after) => sparse({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000, before) },
    7: { 'memoryBudget.tlsAdmissions': check(3, 27_000, 21_000, after) } }, { video: true, updateCheck: true });
  assert.deepEqual(twoChecks(least(27_000, 21_000), least(9000, 21_000)), [['tlsReserveMin', 7000, 9000]]);
  assert.deepEqual(twoChecks(least(15_900, 13_647), least(15_900, 13_000)), [['tlsReserveMin', 7000, 13_000]]);
  // A least value that did not change since the answer before is older, as of a check beside an HTTPS image
  // in an earlier state, and is not judged; after one check it is not read either.
  const earlier = least(15_900, 13_647);
  assert.deepEqual(twoChecks(earlier, earlier), []);
  assert.deepEqual(sparse({ 0: { 'memoryBudget.tlsAdmissions': check(1, 27_000, 21_000, earlier) },
    7: { 'memoryBudget.tlsAdmissions': check(2, 27_000, 21_000, earlier) } }, { video: true, updateCheck: true }), []);
});

test('updateChecksFailed is what the count of refused or unanswered update checks grew by, and fails on both boards', () => {
  const rows = recording(10, { video: true, updateCheck: true });
  // A check that had no answer in 10 s ends in second 4, and one with an answer in second 6.
  const checks = { 4: [[T0 - 6000, 10_000, 'timeout']], 6: [[T0 + 5800, 150, null]] };
  rows.forEach((row, second) => { row.loads = { updateCheck: { checks: checks[second] ?? [] } }; });
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    assert.deepEqual(judge(rows, criteria).failures.map(f => [f.measure, f.t - T0, f.value]), [['updateChecksFailed', 4000, 1]], board);
  }
});

// A recording of video alone, then a series of update checks from second
// `from` to second `to`, then video alone again; free has the Mini's free
// internal RAM in the seconds with a full answer.
function checkSeries(seconds, from, to, free, { beside = {}, always = {}, restartAt = null, checks = 8 } = {}) {
  const rows = recording(seconds, second => (second >= from && second <= to ? { video: true, updateCheck: true, ...always, ...beside } : { video: true, ...always }),
    { restartAt });
  rows.forEach((row, second) => {
    // The Mini has admitted 2 checks before the series, and `checks` more after it.
    const memoryBudget = { ...FULL_STATS.memoryBudget, tlsAdmissions: [{ owner: 'ota-download', admitted: second > to ? 2 + checks : 2, denied: 0 }] };
    row.counters = free[second] === undefined ? null
      : memoryCounters({ ...FULL_STATS, memoryBudget, internalHeapFree: free[second] }, { nowMs: row.device.nowMs });
  });
  return rows;
}

test('memoryLostBytes is the settled free internal RAM before a series of update checks less that after it, at the end of the step after it', () => {
  const lost = (free, options) => judge(checkSeries(40, 15, 29, free, options), rules({ measure: 'memoryLostBytes', max: -1e9 }))
    .failures.map(f => [f.t - T0, f.value]);
  // Before: the highest of the last 10 s (150,000; second 2 is older). After: the highest of the
  // step (148,500), which the Mini reaches when it has freed what the last check held. The
  // answers inside the series, lower by what a check holds, are not counted.
  assert.deepEqual(lost({ 2: 152_000, 7: 147_400, 12: 150_000, 17: 139_000, 22: 139_500, 27: 139_200, 31: 146_000, 36: 148_500, 39: 148_100 }),
    [[39_000, 1500]]);
  // Memory that the Mini has more after the series is no loss.
  assert.deepEqual(lost({ 12: 150_000, 36: 150_400 }), [[39_000, -400]]);
  // No full answer in the step after the series, or in the 10 s before it: the loss is not measured, which fails.
  assert.deepEqual(lost({ 12: 150_000, 22: 139_000 }), [[39_000, Infinity]]);
  assert.deepEqual(lost({ 2: 150_000, 36: 148_500 }), [[39_000, Infinity]]);
  // Four checks are too few for a loss per check to show: not measured. Five are a series.
  assert.deepEqual(lost({ 12: 150_000, 36: 149_900 }, { checks: 4 }), [[39_000, Infinity]]);
  assert.deepEqual(lost({ 12: 150_000, 36: 149_900 }, { checks: 5 }), [[39_000, 100]]);
  // A state with more than the checks and video is no such series.
  assert.deepEqual(lost({ 12: 150_000, 36: 148_500 }, { beside: { httpsMountDisk: true } }), []);
  // MQTT beside the series and beside both steps with video alone is a series; MQTT that comes on
  // with the checks is not, because its connection takes memory that the step before does not have.
  assert.deepEqual(lost({ 12: 150_000, 36: 148_500 }, { always: { mqtt: true } }), [[39_000, 1500]]);
  assert.deepEqual(lost({ 12: 150_000, 36: 148_500 }, { beside: { mqtt: true } }), []);
  // A Mini that restarted in the series starts with other memory.
  assert.deepEqual(lost({ 12: 150_000, 36: 148_500 }, { restartAt: 20 }), []);
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const failed = free => judge(checkSeries(40, 15, 29, free), criteria).failures.map(f => [f.measure, f.t - T0, f.value]);
    assert.deepEqual(failed({ 12: 150_000, 36: 148_977 }), [], board);
    assert.deepEqual(failed({ 12: 150_000, 36: 148_976 }), [['memoryLostBytes', 39_000, 1024]], board);
    assert.deepEqual(failed({ 12: 150_000 }), [['memoryLostBytes', 39_000, Infinity]], board);
  }
});

// A recording with audio on, in which the browser's report of the Mini's
// audio track has these sums in each second; a missing value is the one
// before. Each second the browser plays one second of sound (48000 samples)
// at a level of 0.1, and 50 packets leave its jitter buffer after 60 ms each.
function heard(seconds, sums) {
  const rows = recording(seconds, { video: true, audio: true });
  const now = { packetsLost: 0, concealedSamples: 0, totalAudioEnergy: 0, totalSamplesDuration: 0,
    jitterBufferDelay: 0, jitterBufferEmittedCount: 0 };
  rows.forEach((row, second) => {
    now.totalAudioEnergy += 0.1 * 0.1;
    now.totalSamplesDuration += 1;
    now.jitterBufferDelay += 50 * 0.06;
    now.jitterBufferEmittedCount += 50;
    Object.assign(now, sums[second]);
    row.loads = { audio: { ...now, beepsAt: [], beepClock: null, tones: [] } };
  });
  return rows;
}
const whileAudio = line => rules({ while: { audio: true }, ...line });
const breaches = (rows, line) => judge(rows, whileAudio(line)).failures.map(f => [f.t - T0, Number(f.value.toFixed(3))]);

test('audioPacketsLost and audioConcealedMs are what the browser\'s counts grew by, the second in ms of sound', () => {
  assert.deepEqual(breaches(heard(8, { 3: { packetsLost: 2 }, 6: { packetsLost: 3 } }), { measure: 'audioPacketsLost', max: 0 }),
    [[3000, 2], [6000, 1]]);
  // 960 samples are one packet of 20 ms; 4800 are 100 ms.
  assert.deepEqual(breaches(heard(8, { 2: { concealedSamples: 960 }, 5: { concealedSamples: 5760 } }), { measure: 'audioConcealedMs', max: 0 }),
    [[2000, 20], [5000, 100]]);
  // A report that is missing: what it would have counted is in the next one.
  const rows = heard(8, { 4: { concealedSamples: 960 } });
  rows[4].loads.audio = null;
  assert.deepEqual(breaches(rows, { measure: 'audioConcealedMs', max: 0 }), [[5000, 20]]);
});

test('audioLevel is the level of the sound of that second, not of the session; silence and a browser that plays nothing have 0', () => {
  const quiet = second => 0.1 * 0.1 * second;
  // Second 5 is silent, and in second 7 the browser plays nothing at all.
  const rows = heard(10, { 5: { totalAudioEnergy: quiet(5) }, 6: { totalAudioEnergy: quiet(6) },
    7: { totalAudioEnergy: quiet(6), totalSamplesDuration: 7 }, 8: { totalAudioEnergy: quiet(7), totalSamplesDuration: 8 } });
  assert.deepEqual(breaches(rows, { measure: 'audioLevel', min: 0.05 }), [[5000, 0], [7000, 0]]);
  assert.equal(levelBetween({ totalAudioEnergy: 0.5, totalSamplesDuration: 12 }, { totalAudioEnergy: 0.1, totalSamplesDuration: 2 }), 0.2);
  // A report of a browser without the sums says nothing about the level.
  assert.equal(levelBetween({}, { totalAudioEnergy: 0.1, totalSamplesDuration: 2 }), undefined);
  assert.equal(levelBetween(null, null), undefined);
});

test('the tool mutes its own audio element only while an element of the UI plays the same track with sound', () => {
  const track = { kind: 'audio' };
  const stream = tracks => ({ getAudioTracks: () => tracks });
  const element = fields => ({ paused: false, muted: false, srcObject: stream([track]), ...fields });
  // The UI's audio element for a session that connected with audio enabled.
  assert.equal(playsTrack([element({})], track), true);
  // The UI's video element has the track in its stream and is muted; its
  // audio element does not play when the browser refused autoplay.
  assert.equal(playsTrack([element({ muted: true }), element({ paused: true })], track), false);
  // An element with another session's track, one with a stream of video
  // alone, one with a file, and a page without elements.
  assert.equal(playsTrack([element({ srcObject: stream([{ kind: 'audio' }]) }), element({ srcObject: stream([]) }),
    element({ srcObject: null })], track), false);
  assert.equal(playsTrack([], track), false);
  // One playing element among others is enough.
  assert.equal(playsTrack([element({ muted: true }), element({})], track), true);
});

test('audioJitterBufferMs is the mean wait of the sound played in that second', () => {
  // In second 4 the 50 packets wait 200 ms each; the mean of the session so far is 88 ms.
  const before = 4 * 50 * 0.06;
  const rows = heard(8, { 4: { jitterBufferDelay: before + 50 * 0.2 }, 5: { jitterBufferDelay: before + 50 * 0.2 + 50 * 0.06 } });
  assert.deepEqual(breaches(rows, { measure: 'audioJitterBufferMs', max: 100 }), [[4000, 200]]);
  // No value in a second in which nothing left the buffer.
  const stopped = heard(8, { 4: { jitterBufferEmittedCount: 200 }, 5: { jitterBufferEmittedCount: 250 } });
  assert.deepEqual(breaches(stopped, { measure: 'audioJitterBufferMs', min: 1 }), []);
});

test('audioDelayMs is the longest delay of the beeps heard in that second, by the sample\'s clocks, and has no value without a beep or a clock', () => {
  const rows = heard(8, {});
  // This machine's clock is 5000 ms ahead of the host's, and the page's second 0 is at 700 000 on the host's clock.
  const beepClock = { ahead: 5000, zero: 700_000 };
  const heardAt = (second, delayMs) => 700_000 + second * 1000 + 5000 + delayMs;
  Object.assign(rows[3].loads.audio, { beepsAt: [heardAt(3, 140)], beepClock });
  // A sample of the browser that came late: its beep is in the next sample beside that one's.
  Object.assign(rows[5].loads.audio, { beepsAt: [heardAt(4, 420.4), heardAt(5, 180)], beepClock });
  // Before the target page's first report with a clock a beep has no delay.
  Object.assign(rows[6].loads.audio, { beepsAt: [heardAt(6, 900)], beepClock: null });
  assert.deepEqual(breaches(rows, { measure: 'audioDelayMs', max: 300 }), [[5000, 420]]);
  assert.deepEqual(breaches(rows, { measure: 'audioDelayMs', min: 200 }), [[3000, 140]]);
});

test('audioFramesDropped is what the Mini\'s count of dropped Opus frames grew by; firmware without the count has no value', () => {
  const rows = heard(8, {});
  rows.forEach((row, second) => { row.device.audio = { droppedFrames: second >= 6 ? 7 : second >= 3 ? 2 : 0 }; });
  assert.deepEqual(breaches(rows, { measure: 'audioFramesDropped', max: 0 }), [[3000, 2], [6000, 5]]);
  assert.deepEqual(breaches(heard(8, {}), { measure: 'audioFramesDropped', min: 1 }), []);
});

test('audioPausedByUpload fails when the Mini does not hold the sound off for the upload, and when it still does after it; old firmware has no value', () => {
  const criteria = rules(
    { measure: 'audioPausedByUpload', max: 0 },
    { while: { upload: true }, measure: 'audioPausedByUpload', min: 1 });
  const uploading = second => second >= 3 && second <= 8;
  const failuresWith = pausedBy => {
    const rows = recording(12, second => ({ video: true, upload: uploading(second) }));
    rows.forEach((row, second) => { row.device.audioPausedBy = pausedBy(second); });
    return judge(rows, criteria).failures.map(f => [f.t, f.value]);
  };
  assert.deepEqual(failuresWith(second => (uploading(second) ? ['upload'] : [])), []);
  assert.deepEqual(failuresWith(() => []), [[T0 + 3000, 0]]);
  assert.deepEqual(failuresWith(second => (second >= 3 ? ['upload'] : [])), [[T0 + 9000, 1]]);
  // Firmware that holds only video off for an upload says nothing about the sound.
  const old = recording(12, second => ({ video: true, upload: uploading(second) }));
  old.forEach((row, second) => { row.device.videoPausedBy = uploading(second) ? ['upload'] : []; });
  assert.equal(judge(old, criteria).result, 'pass');
});

test('the measures of the sound have no value while the Mini holds the sound off, also in the seconds without its answer around the hold, and count again after', () => {
  // The Mini holds the sound off in seconds 3 to 7. As during an upload over
  // Wi-Fi, it does not answer when the hold begins, in seconds 3 and 4, nor
  // in second 6, and its first answer without the hold is in second 8. The
  // browser gets no packet during the hold: it makes up every sample, plays
  // silence, and nothing leaves its jitter buffer. In second 8 the sound
  // comes back after 200 ms.
  const held = second => second >= 3 && second <= 7;
  const sums = {};
  let concealed = 0;
  let energy = 0;
  let emitted = 0;
  // With the sound back, seconds 9 and 11 are silent and have 100 ms of
  // made-up sound; the Mini does not answer in second 11.
  const gap = second => second === 9 || second === 11;
  for (let second = 0; second < 13; second++) {
    if (held(second)) concealed += 48000;
    else if (!gap(second)) energy += 0.1 * 0.1;
    if (!held(second)) emitted += 50;
    if (second === 8) concealed += 9600;
    if (gap(second)) concealed += 4800;
    sums[second] = { concealedSamples: concealed, totalAudioEnergy: energy,
      jitterBufferEmittedCount: emitted, jitterBufferDelay: emitted * 0.06 };
  }
  const rows = heard(13, sums);
  rows.forEach((row, second) => {
    row.device = [3, 4, 6, 11].includes(second) ? null : { ...row.device, audioPausedBy: held(second) ? ['upload'] : [] };
    const delayMs = held(second) || second === 8 ? 900 : 100;
    Object.assign(row.loads.audio, { beepsAt: [700_000 + second * 1000 + 5000 + delayMs], beepClock: { ahead: 5000, zero: 700_000 } });
  });
  // The seconds after the hold count again, also one without an answer of the Mini.
  assert.deepEqual(breaches(rows, { measure: 'audioConcealedMs', max: 20 }), [[9000, 100], [11000, 100]]);
  assert.deepEqual(breaches(rows, { measure: 'audioLevel', min: 0.04 }), [[9000, 0], [11000, 0]]);
  assert.deepEqual(breaches(rows, { measure: 'audioDelayMs', max: 250 }), []);
  assert.deepEqual(breaches(rows, { measure: 'audioJitterBufferMs', max: 59 }), [[1000, 60]]);
  // The same recording from a Mini that reports no hold: the silence counts from its first second.
  rows.forEach(row => { if (row.device) delete row.device.audioPausedBy; });
  assert.deepEqual(breaches(rows, { measure: 'audioConcealedMs', max: 20 }), [[3000, 1000], [11000, 100]]);
});

test('without an answer of the Mini during a whole upload, the sound counts as held off in the seconds in which the browser got no audio packet', () => {
  // The upload is on in seconds 5 to 14, and the Mini answers none of the
  // stats requests of that time. received(second): the audio packets that
  // the browser got in that second, of 50; it makes up the rest.
  const uploading = second => second >= 5 && second <= 14;
  const run = (received, { criteria, answers = () => null } = {}) => {
    const sums = {};
    let packets = 0;
    let concealed = 0;
    for (let second = 0; second < 20; second++) {
      packets += received(second);
      concealed += (50 - received(second)) * 960;
      sums[second] = { packetsReceived: packets, concealedSamples: concealed };
    }
    const rows = heard(20, sums);
    rows.forEach((row, second) => {
      row.state = { video: true, audio: true, upload: uploading(second) };
      const pausedBy = uploading(second) ? answers(second) : [];
      row.device = pausedBy === null ? null : { ...row.device, audioPausedBy: pausedBy };
    });
    return judge(rows, criteria ?? { everySecond: [
      { measure: 'audioPausedByUpload', max: 0 },
      { while: { upload: true }, measure: 'audioPausedByUpload', min: 1 },
      { while: { audio: true }, measure: 'audioConcealedMs', max: 40 },
    ] }).failures.map(f => [f.measure, f.t - T0, Math.round(f.value)]);
  };
  // No answer and no packet: the Mini holds the sound off. The first second
  // of the upload still has 40 packets, and what it lacks is in the pause of
  // the walk's change, which this recording does not have.
  const held = second => (second === 5 ? 40 : uploading(second) ? 0 : 50);
  assert.deepEqual(run(held), [['audioConcealedMs', 5000, 200]]);
  // No answer, and a sound that goes on with a gap of 200 ms in second 8: the gap counts.
  assert.deepEqual(run(second => (second === 8 ? 40 : 50)), [['audioConcealedMs', 8000, 200]]);
  // With answers, the Mini's report decides as before: a Mini that reports
  // the hold has no gap, and one that reports none has the silence as a gap
  // and fails the rule for its report.
  assert.deepEqual(run(held, { answers: () => ['upload'] }), []);
  const silentFromSecond6 = second => (uploading(second) && second > 5 ? 0 : 50);
  assert.deepEqual(run(silentFromSecond6, { answers: () => [] }), [['audioPausedByUpload', 5000, 0], ['audioConcealedMs', 6000, 1000]]);
  // A board whose rules expect no hold during an upload: silence without an answer is a gap.
  const noHold = { everySecond: [{ while: { audio: true }, measure: 'audioConcealedMs', max: 40 }] };
  assert.deepEqual(run(silentFromSecond6, { criteria: noHold }), [['audioConcealedMs', 6000, 1000]]);
  assert.deepEqual(run(silentFromSecond6), []);
});

// The target page's sound at a sample rate: a tone of 220 Hz at 0.05, and
// from each start (in seconds) a beep of 1000 Hz at 0.25, 100 ms long with
// ramps of 5 ms. volume is what the host has set for the Mini's output.
function pageSoundSamples(sampleRate, seconds, beepStarts, volume = 1) {
  const samples = new Float32Array(Math.round(sampleRate * seconds));
  for (let i = 0; i < samples.length; i++) {
    const t = i / sampleRate;
    const since = beepStarts.map(start => t - start).find(time => time >= 0 && time < 0.1);
    const beep = since === undefined ? 0 : 0.25 * Math.min(1, since / 0.005, (0.1 - since) / 0.005);
    samples[i] = volume * (0.05 * Math.sin(2 * Math.PI * 220 * t) + beep * Math.sin(2 * Math.PI * 1000 * t));
  }
  return samples;
}
test('the strongest frequency of the target page\'s sound is its tone\'s 220 Hz in a window without a beep and 1000 Hz in one with a beep, at any volume, and silence has none', () => {
  for (const sampleRate of [48_000, 44_100]) {
    const bin = sampleRate / 4096;
    // 3 s with a beep at each whole second, in blocks of 1024 samples as the page's tap gets them; every window is looked at.
    const find = toneFinder(sampleRate, 4096, 1);
    const sound = pageSoundSamples(sampleRate, 3, [0, 1, 2], 0.3);
    const found = [];
    for (let from = 0; from < sound.length; from += 1024) found.push(...find(sound.subarray(from, from + 1024)));
    assert.equal(found.length, Math.floor(sound.length / 4096), `${sampleRate}`);
    // A window of 85 ms that lies in a beep of 100 ms has the beep's frequency; the others the tone's.
    const beeps = found.filter(hz => Math.abs(hz - 1000) <= bin);
    const tones = found.filter(hz => Math.abs(hz - 220) <= bin);
    assert.equal(beeps.length + tones.length, found.length, `${sampleRate}: ${found.join(' ')}`);
    assert.ok(beeps.length >= 3 && beeps.length <= 6, `${sampleRate}: ${beeps.length} windows with a beep`);
    // One window in four, as the page uses it: the middle one of a second's three is the tone.
    const every4 = toneFinder(sampleRate);
    const second = [];
    for (let from = 0; from < sampleRate; from += 1024) second.push(...every4(sound.subarray(from, from + 1024)));
    assert.ok(second.length >= 2 && second.length <= 3, `${sampleRate}: ${second.length} windows in a second`);
    assert.ok(Math.abs(second.sort((a, b) => a - b)[Math.floor((second.length - 1) / 2)] - 220) <= bin, `${sampleRate}`);
    // A tone of another pitch is found at its own frequency.
    const other = Float32Array.from({ length: 4096 }, (unused, i) => 0.05 * Math.sin(2 * Math.PI * 440 * i / sampleRate));
    assert.ok(Math.abs(toneFinder(sampleRate, 4096, 1)(other)[0] - 440) <= bin, `${sampleRate}`);
    assert.deepEqual(toneFinder(sampleRate, 4096, 1)(new Float32Array(4096)), [null]);
  }
});

// The starts that the detector finds, in ms, when it gets the sound in blocks of 1024 samples.
function beepStartsMs(sampleRate, samples) {
  const detect = beepDetector(sampleRate);
  const starts = [];
  for (let from = 0; from < samples.length; from += 1024) starts.push(...detect(samples.subarray(from, from + 1024)));
  return starts.map(index => index * 1000 / sampleRate);
}

test('the start of each beep in the sound is found to 5 ms, at every volume of the host and at both usual sample rates', () => {
  const starts = [0.4321, 1.4321, 2.4321];
  for (const sampleRate of [48000, 44100]) {
    for (const volume of [1, 0.06, 0.004]) {
      const found = beepStartsMs(sampleRate, pageSoundSamples(sampleRate, 3, starts, volume));
      assert.equal(found.length, 3, `${sampleRate} Hz, volume ${volume}: ${found}`);
      found.forEach((ms, index) => assert.ok(Math.abs(ms - starts[index] * 1000) <= 5, `${sampleRate} Hz, volume ${volume}: ${found}`));
    }
  }
});

test('the tone alone, silence, and a tone as loud as the beep have no beep; a beep with a hole in it is one beep', () => {
  assert.deepEqual(beepStartsMs(48000, pageSoundSamples(48000, 3, [])), []);
  assert.deepEqual(beepStartsMs(48000, new Float32Array(48000)), []);
  const loudTone = Float32Array.from({ length: 48000 }, (_, i) => 0.25 * Math.sin(2 * Math.PI * 220 * i / 48000));
  assert.deepEqual(beepStartsMs(48000, loudTone), []);
  // 20 ms of the beep are silence, as when a packet is lost and nothing fills it.
  const holed = pageSoundSamples(48000, 2, [0.5]);
  holed.fill(0, 48000 * 0.54, 48000 * 0.56);
  const found = beepStartsMs(48000, holed);
  assert.equal(found.length, 1);
  assert.ok(Math.abs(found[0] - 500) <= 5, String(found));
});

test('the delay of a beep is taken on the host\'s clock, whatever the two clocks differ by and however late a report comes', () => {
  // This machine's clock is 5000 ms ahead of the host's. The page's second 0
  // is at 700 000 on the host's clock, and a report takes 1 to 9 ms.
  const clock = beepClock();
  assert.equal(clock.delayMs(1_000_000), null);
  assert.equal(clock.now(), null);
  const report = (seconds, way) => clock.report({ seconds, clock: 700_000 + seconds * 1000, at: 700_000 + seconds * 1000 + 5000 + way });
  report(10.2, 9);
  report(11.2, 1);
  report(12.2, 4);
  // The beep of the page's second 13 is heard 180 ms after the host played it.
  const heardAt = 700_000 + 13_000 + 5000 + 180;
  // The fastest report took 1 ms, which the delay is short by.
  assert.equal(clock.delayMs(heardAt), 179);
  // The recording keeps the two clocks beside the beep, and the judge takes the same delay from them.
  assert.deepEqual(clock.now(), { ahead: 5001, zero: 700_000 });
  assert.equal(beepDelayMs(heardAt, clock.now()), 179);
  // A page that says no clock, as one of an earlier version of the tool, changes nothing.
  clock.report({ seconds: 14.2, at: 0 });
  assert.equal(clock.delayMs(heardAt), 179);
  // A second later the same beep's place in the second is the same delay.
  assert.equal(clock.delayMs(heardAt + 1000), 179);
});

test('a change that takes the Mini off USB starts only when the page has reported its sound off, and the page plays again after it', async () => {
  const log = [];
  let playing = false;
  const page = { want: on => { log.push(`want ${on}`); setTimeout(() => { playing = on; }, 30); }, playing: () => playing };
  const sound = pageSound({ ...page, waitMs: 500, pollMs: 5 });
  // Without audio in the list the change runs at once, and the page is not asked for anything new.
  assert.equal(await sound.offUsb(async () => { log.push(`mount, playing ${playing}`); return 'mounted'; }), 'mounted');
  assert.deepEqual(log.splice(0), ['mount, playing false', 'want false']);

  sound.turn(true);
  await new Promise(resolve => setTimeout(resolve, 60));
  assert.equal(playing, true);
  await sound.offUsb(async () => { log.push(`mount, playing ${playing}`); });
  assert.deepEqual(log.splice(0), ['want true', 'want false', 'mount, playing false', 'want true']);
  // Also after a change that fails.
  await assert.rejects(sound.offUsb(async () => { throw new Error('the Mini refused'); }), /the Mini refused/);
  assert.equal(log.at(-1), 'want true');
});

test('a page that does not report its sound off stops the change, and a page without a report counts as playing', async () => {
  for (const playing of [() => true, () => null]) {
    let changed = false;
    const wishes = [];
    const sound = pageSound({ want: on => wishes.push(on), playing, waitMs: 100, pollMs: 5 });
    sound.turn(true);
    await assert.rejects(sound.offUsb(async () => { changed = true; }), /has not reported its sound off after 0.1 s/);
    assert.equal(changed, false);
    // The list still has audio on: the page keeps its wish.
    assert.deepEqual(wishes, [true, false, true]);
  }
});

test('the page\'s open request for the sound is answered when the tool\'s wish changes, and at once when the page has another wish', async () => {
  const server = await serve('127.0.0.1', null);
  try {
    const ask = wanted => fetch(`${server.base}/target-page/sound?wanted=${wanted}`).then(answer => answer.json());
    // The page is silent and the tool wants that: the request stays open.
    let answer = null;
    const open = ask(false).then(wish => { answer = wish; });
    await new Promise(resolve => setTimeout(resolve, 200));
    assert.equal(answer, null);
    server.wantSound(true);
    await open;
    assert.deepEqual(answer, { sound: true, noise: false });
    // A page that still has the old wish learns the new one at once.
    assert.deepEqual(await ask(false), { sound: true, noise: false });
    server.wantSound(false);
    assert.deepEqual(await ask(true), { sound: false, noise: false });
  } finally {
    server.close();
  }
});

test('a cloud connection that is gone fails in that second, while cloud is on', () => {
  const criteria = rules({ while: { cloud: true }, measure: 'cloudConnected', min: 1 });
  const rows = connected => recording(10, { video: true, cloud: true }).map((row, second) =>
    ({ ...row, loads: { cloud: { connected: connected(second) } } }));
  assert.equal(judge(rows(() => true), criteria).result, 'pass');
  assert.equal(judge(recording(10, { video: true }), criteria).result, 'pass');
  assert.deepEqual(judge(rows(second => second < 5), criteria).failures.map(f => f.t), [T0 + 5000]);
});

test('while tailscale is on, the floors of the terminal\'s and the file transfer\'s rates are 80 % of the LAN floors on Ethernet and 70 % on Wi-Fi, and they apply in place of them', () => {
  for (const [board, part] of [['ethernet', 0.8], ['wifi', 0.7]]) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const rates = ['terminalBytesPerSecond', 'fileBytesPerSecond'];
    const only = { everySecond: criteria.everySecond.filter(rule => rates.includes(rule.measure)) };
    for (const lan of only.everySecond.filter(rule => !rule.while.tailscale)) {
      const state = { video: true, osServices: true, ...lan.while, tailscale: true };
      const load = lan.measure === 'terminalBytesPerSecond' ? 'osTerminal' : 'osFiles';
      const field = load === 'osTerminal' ? 'bytes' : 'bytesSent';
      // The other rate of the state is far above every floor.
      const rows = share => recording(8, state).map((row, second) => ({ ...row, loads: {
        osTerminal: { bytes: second * 10_000_000, breaks: [] }, osFiles: { bytesSent: second * 10_000_000 },
        [load]: { [field]: Math.round(second * lan.min * share), ...(load === 'osTerminal' ? { breaks: [] } : {}) } } }))
        .map(row => (state.osFiles ? row : { ...row, loads: { [load]: row.loads[load] } }));
      const at = share => judge(rows(share), only);
      assert.equal(at(part + 0.05).result, 'pass', `${board} ${JSON.stringify(lan.while)}: just above the tunnel's part of the LAN floor passes`);
      assert.deepEqual(at(part + 0.05).undecided, [], board);
      assert.equal(at(part - 0.05).result, 'fail', `${board} ${JSON.stringify(lan.while)}: just under the tunnel's part of the LAN floor fails`);
      assert.deepEqual(new Set(at(part - 0.05).failures.map(f => f.measure)), new Set([lan.measure]), board);
    }
  }
});

test('a Mini that leaves the tailnet, or a page that is not at its tailnet address, fails in that second, while tailscale is on', () => {
  const criteria = rules({ while: { tailscale: true }, measure: 'tailscaleRunning', min: 1 });
  const rows = load => recording(10, { video: true, tailscale: true }).map((row, second) =>
    ({ ...row, loads: { tailscale: load(second) } }));
  const on = { running: true, address: '100.64.0.7', path: 'direct', onTailnet: true };
  assert.equal(judge(rows(() => on), criteria).result, 'pass');
  assert.deepEqual(judge(rows(second => ({ ...on, running: second < 5 })), criteria).failures.map(f => f.t), [T0 + 5000]);
  assert.deepEqual(judge(rows(second => ({ ...on, onTailnet: second !== 3 })), criteria).failures.map(f => f.t), [T0 + 3000]);
  // A second without the Mini's answer has no value.
  assert.equal(judge(rows(second => ({ ...on, running: second === 4 ? null : true })), criteria).result, 'pass');
});

test('the tailscale switch joins with the key of its file, moves the page to the tailnet address, and removes the key when it goes off', async () => {
  const keyFile = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'tailscale-')), 'key');
  fs.writeFileSync(keyFile, 'tskey-auth-test\n');
  const calls = [];
  let running = false;
  let at = 'http://192.168.1.100/';
  const page = { url: () => at, goto: async url => { at = url; } };
  const rpc = async (method, params) => {
    calls.push([method, params]);
    if (method === 'setTailscaleConfig') running = params.enabled;
    if (method === 'getTailscaleStatus') return running ? { running: true, self: { tailscaleIPs: ['100.64.0.7'] } } : { running: false, backendState: 'Stopped' };
    return null;
  };
  // This machine has the Mini as a peer from the second question on.
  let asked = 0;
  const sw = tailscaleSwitch({ page, url: 'http://192.168.1.100/', rpc, keyFile, path: async () => (++asked > 1 ? 'direct' : null) });
  await sw.prepare();
  await sw.on();
  assert.deepEqual(calls[0], ['setTailscaleConfig', { enabled: true, authKey: 'tskey-auth-test' }]);
  assert.equal(at, 'http://100.64.0.7/');
  assert.equal(await sw.isOn(), true);
  assert.deepEqual({ ...await sw.sample() }, { running: true, address: '100.64.0.7', path: 'direct', onTailnet: true });
  await sw.off();
  assert.equal(at, 'http://192.168.1.100/');
  assert.deepEqual(calls.at(-1), ['setTailscaleConfig', { enabled: false, authKey: '' }]);
  assert.equal(await sw.isOn(), false);
  // A firmware without Tailscale has no such method, and off still works.
  const old = tailscaleSwitch({ page, url: 'http://192.168.1.100/', keyFile,
    rpc: async () => { throw new Error('Method not found'); } });
  await old.off();
  assert.equal(await old.isOn(), false);
  await assert.rejects(tailscaleSwitch({ page, url: '', rpc, keyFile: keyFile + '-missing' }).prepare(), /does not exist/);
});

test('an adoption has its time once, from the request for the Mini\'s /adopt page to the name page, and a second adoption has its own', () => {
  const criteria = rules({ while: { cloud: true }, measure: 'adoptMs', max: 2000 });
  // The cloud is on from second 2 to 9 and from second 12. adoptions: for
  // each, the second in which the browser asked for /adopt and its time.
  const rows = adoptions => recording(20, second => ({ video: true, cloud: (second >= 2 && second < 10) || second >= 12 })).map((row, second) => {
    if (!row.state.cloud) return row;
    const done = adoptions.findLast(([at, ms]) => at * 1000 + ms <= second * 1000 && (second < 10) === (at < 10));
    const adoption = done ? { adoptAt: T0 + done[0] * 1000, registeredAt: T0 + done[0] * 1000 + done[1] - 20, namePageAt: T0 + done[0] * 1000 + done[1], socketAt: null } : {};
    return { ...row, loads: { cloud: { connected: true, ...adoption } } };
  });
  assert.equal(judge(rows([[2, 900], [12, 1900]]), criteria).result, 'pass');
  // The first adoption is over its limit: one line, in the first second that has its arrival, and none in the seconds after.
  assert.deepEqual(judge(rows([[2, 2600], [12, 600]]), criteria).failures.map(f => [f.t, f.measure, f.value]), [[T0 + 5000, 'adoptMs', 2600]]);
  // The second one is: it has its own line, although the samples before it had the first adoption's times.
  assert.deepEqual(judge(rows([[2, 600], [12, 3100]]), criteria).failures.map(f => [f.t, f.value]), [[T0 + 16000, 3100]]);
  // A cloud that is on without an adoption in the recording, as after an update, has no value.
  assert.equal(judge(rows([]), criteria).result, 'pass');
  // An entry for the change tolerates the one second that has the value.
  const entry = { issue: 'https://example.test/issues/9', change: { cloud: true }, measure: 'adoptMs', max: 3000, tolerateSeconds: 1, recoverySeconds: 2 };
  const withChange = adoptions => [change('cloud', true, 2, 3, { state: { video: true } }), ...rows(adoptions)];
  assert.equal(judge(withChange([[2, 2600]]), criteria, [entry]).result, 'pass');
  assert.equal(judge(withChange([[2, 3600]]), criteria, [entry]).result, 'fail');
});

test('a count that grows beside a missing report is counted in the next report', () => {
  // A key that the host did not get, sent at 1.5 s and judged in second 5.
  const rows = withHid(recording(10, { video: true, hid: true }), { sends: [[0x04, T0 + 1500]] });
  const lost = () => judge(rows, rules({ while: { hid: true }, measure: 'keysLost', max: 0 })).failures.map(f => [f.t, f.value]);
  assert.deepEqual(lost(), [[T0 + 5000, 1]]);
  // The switch's sample of second 5 comes too late for its row: the recorder puts its events into the
  // sample of second 6, and the key is counted there.
  rows[6].loads.hid = withLate(rows[5].loads.hid, rows[6].loads.hid);
  rows[5].loads.hid = null;
  assert.deepEqual(lost(), [[T0 + 6000, 1]]);
});

test('a rate is taken over the time between the reports, not between the samples', () => {
  // 30 frames a second; the browser answers 900, 600, 300 and 0 ms late, so
  // each report has 30 % fewer frames than a second's worth, over 0.7 s.
  const rows = recording(10, { video: true });
  const late = second => Math.max(0, 900 - 300 * Math.max(0, second - 4));
  rows.forEach((row, second) => {
    const timestamp = row.t + (second < 4 ? 0 : late(second));
    row.browser.video = { framesDecoded: Math.round((timestamp - T0) * 0.03), timestamp };
  });
  assert.equal(judge(rows, rules({ while: { video: true }, measure: 'framesPerSecond', min: 27 })).result, 'pass');
});

test('a counter that is gone counts as no progress', () => {
  assert.equal(judge(recording(20, { video: true }, { noPeer: range(5, 19) }), rules(VIDEO)).failures.length, 1);
  const reads = rules({ while: { mount: true }, measure: 'mountReadBytesPerSecond', min: 1, failAfterSeconds: 10 });
  assert.equal(judge(recording(30, { mount: true }, { noRead: range(5, 14) }), reads).result, 'pass');
  assert.equal(judge(recording(30, { mount: true }, { noRead: range(5, 29) }), reads).result, 'fail');
});

test('mountReadErrors is what the host\'s count of failed commands for the mounted medium grew by, and has no value on a host without the count', () => {
  const rule = rules({ while: { mount: true }, measure: 'mountReadErrors', max: 0 });
  const rows = errors => {
    const all = recording(8, { mount: true });
    all.forEach((row, second) => { if (errors) row.loads.mount.readErrors = errors[second]; });
    return all;
  };
  assert.deepEqual(judge(rows([2, 2, 2, 5, 5, 5, 6, 6]), rule).failures.map(f => [f.t - T0, f.value]), [[3000, 3], [6000, 1]]);
  assert.equal(judge(rows(null), rule).result, 'pass');
});

test('noisePicture cannot tell without a recent page report, so turning it off does not finish then', () => {
  const screen = { screenWidth: 2560, screenHeight: 1440 };
  assert.equal(noisePictureOn(null), null);
  assert.equal(noisePictureOn({ noise: false, width: 2560, height: 1394, ...screen }), false);
  assert.equal(noisePictureOn({ noise: true, width: 2560, height: 1394, ...screen }), true);
  assert.equal(noisePictureOn({ noise: true, width: 1280, height: 720, ...screen }), null);
});

test('updateCheck asks again after each answer, works from the first answer on, and stops when it goes off', async () => {
  const answers = [new Error('no route'), null, null];
  let asked = 0;
  const check = async () => {
    const answer = answers[Math.min(asked++, answers.length - 1)];
    if (answer) throw answer;
  };
  const updates = updateCheckSwitch(check, () => 5);
  assert.equal(updates.isOn(), false);
  assert.equal(updates.sample(), null);
  await updates.on();
  assert.equal(updates.isOn(), true);
  await until(() => asked >= 1, 1000, 'no check');
  // The first check failed: the change is not finished, and says why.
  assert.equal(updates.works(true), 'the Mini has answered no update check: no route');
  await until(() => updates.works(true) === true, 1000, 'no answered check');
  // Each sample has the checks that ended since the one before.
  const ended = [];
  const take = () => {
    const sample = updates.sample();
    ended.push(...sample.checks);
    return sample;
  };
  try {
    await until(() => take().answered >= 2, 1000, 'no second answered check');
    const { checks, ...counts } = updates.sample();
    ended.push(...checks);
    assert.deepEqual({ ...counts, answered: 0 }, { answered: 0, failed: 1, error: 'no route' });
    // Each check once, with its start, its time and its failure.
    assert.equal(ended.length, counts.answered + counts.failed);
    assert.deepEqual(ended.map(([startedAt, ms, error]) => [typeof startedAt, ms >= 0, error]),
      [['number', true, 'no route'], ...Array(counts.answered).fill(['number', true, null])]);
  } finally {
    await updates.off();
  }
  assert.equal(updates.isOn(), false);
  const then = asked;
  await new Promise(resolve => setTimeout(resolve, 50));
  assert.ok(asked <= then + 1, `${asked - then} checks after off`);
});

test('updateCheck waits before each next check as long as its pause is at that time', async () => {
  let asked = 0;
  let pauseMs = 60_000;
  const updates = updateCheckSwitch(async () => { asked++; }, () => pauseMs);
  await updates.on();
  await until(() => updates.sample().answered === 1, 1000, 'no answered check');
  await new Promise(resolve => setTimeout(resolve, 100));
  assert.equal(asked, 1);
  await updates.off();
  // A short pause: the checks follow each other.
  pauseMs = 5;
  await updates.on();
  await until(() => updates.sample().answered >= 3, 1000, 'no third check');
  await updates.off();
});

test('the image is served over TLS as well, to a client that trusts the test CA', async () => {
  const server = await serve('127.0.0.1', null);
  try {
    const { url } = await server.secure();
    assert.match(url, /^https:\/\/127\.0\.0\.1:\d+\/combined-load\.iso$/);
    assert.equal((await server.secure()).url, url);
    const part = await ask(url, { headers: { Range: 'bytes=4096-4159' } });
    assert.equal(part.status, 206);
    assert.equal(part.body, (await (await fetch(server.url, { headers: { Range: 'bytes=4096-4159' } })).text()));
    // The image's bytes are not text; the plain server's answer for the same range has 64 of them.
    assert.equal((await (await fetch(server.url, { headers: { Range: 'bytes=4096-4159' } })).arrayBuffer()).byteLength, 64);
    await assert.rejects(ask(url, { ca: tls.rootCertificates }));
  } finally {
    server.close();
  }
});

// A mount beside OS Services video is known to take up to 12 s.
const SLOW_MOUNT = { issue: 'https://example.test/issues/1', change: { mount: true }, while: { osServices: true }, max: 12 };
const mount = (seconds, osServices) => [change('mount', true, 0, seconds, { state: { video: true, osServices } }),
  ...recording(20, { video: true, osServices, mount: true })];

test('a change that a known failure covers is reported as known and does not fail the run', () => {
  const { result, failures, known, notOccurred } = judge(mount(11, true), MOUNT_LIMIT, [SLOW_MOUNT]);
  assert.equal(result, 'pass');
  assert.deepEqual([failures.length, notOccurred.length], [0, 0]);
  assert.equal(known.length, 1);
  assert.deepEqual(known[0].entries, [SLOW_MOUNT]);
  assert.equal(known[0].seconds, 11);
});

test('a change that is worse than its known failure fails the run and names the entry', () => {
  const { result, failures, known } = judge(mount(13, true), MOUNT_LIMIT, [SLOW_MOUNT]);
  assert.equal(result, 'fail');
  assert.deepEqual(failures[0].worseThan, [SLOW_MOUNT]);
  assert.equal(known.length, 0);
});

test('a known failure covers only its own state', () => {
  const { result, failures } = judge(mount(11, false), MOUNT_LIMIT, [SLOW_MOUNT]);
  assert.equal(result, 'fail');
  assert.deepEqual(failures[0].worseThan, []);
});

test('a known failure that did not occur where it applies is a note, and the run passes', () => {
  const { result, failures, notOccurred } = judge(mount(4, true), MOUNT_LIMIT, [SLOW_MOUNT]);
  assert.equal(result, 'pass');
  assert.equal(failures.length, 0);
  assert.deepEqual(notOccurred, [SLOW_MOUNT]);
});

test('a known failure whose situation is not in the recording is ignored', () => {
  const { result, notOccurred } = judge(mount(4, false), MOUNT_LIMIT, [SLOW_MOUNT]);
  assert.equal(result, 'pass');
  assert.equal(notOccurred.length, 0);
});

test('a known failure of an every-second rule tolerates a breach up to its tolerateSeconds', () => {
  const entry = { issue: 'https://example.test/issues/2', while: { ota: true }, measure: 'framesPerSecond', tolerateSeconds: 6 };
  const run = frozen => judge(recording(30, { video: true, ota: true }, { frozen }), rules(VIDEO), [entry]);
  assert.equal(run(range(5, 10)).result, 'pass');
  assert.equal(run(range(5, 10)).known[0].seconds, 6);
  assert.deepEqual(run(range(5, 11)).failures[0].worseThan, [entry]);
  assert.deepEqual(run([]).notOccurred, [entry]);
});

test('an entry with a host applies to a recording of that host, and to no other and to none without a host', () => {
  const frozen = { issue: 'https://example.test/issues/3', host: 'windows', while: { video: true }, measure: 'framesPerSecond', tolerateSeconds: 6 };
  const slow = { ...SLOW_MOUNT, host: 'windows' };
  // The recording's first row, with or without the host; an older recording has no such row.
  const run = first => judge([...first, ...recording(30, { video: true }, { frozen: range(5, 10) }), ...mount(11, true)],
    { ...rules(VIDEO), ...MOUNT_LIMIT }, [frozen, slow]);
  const onWindows = run([{ t: T0, run: { board: 'wifi', host: 'windows' } }]);
  assert.equal(onWindows.result, 'pass');
  assert.deepEqual(onWindows.known.map(k => k.entries), [[slow], [frozen]]);
  assert.equal(report(onWindows, T0).split('\n')[0], 'HOST  windows');
  for (const first of [[{ t: T0, run: { board: 'wifi', host: 'linux' } }], [{ t: T0, run: { board: 'wifi' } }], []]) {
    const elsewhere = run(first);
    assert.deepEqual(elsewhere.failures.map(f => [f.measure ?? 'mount', f.worseThan]), [['framesPerSecond', []], ['mount', []]]);
    assert.deepEqual([elsewhere.known, elsewhere.notOccurred], [[], []]);
  }
  assert.equal(report(run([{ t: T0, run: { board: 'wifi', host: 'linux' } }]), T0).split('\n')[0], 'HOST  linux');
  assert.match(report(run([]), T0).split('\n')[0], /^HOST {2}none in the recording/);
  // An entry without a host applies on each of them.
  const { host, ...everywhere } = frozen;
  assert.deepEqual(run([{ t: T0, run: { board: 'wifi', host: 'macos' } }]).failures.map(f => f.measure ?? 'mount'), ['framesPerSecond', 'mount']);
  assert.equal(judge([{ t: T0, run: { host: 'macos' } }, ...recording(30, { video: true }, { frozen: range(5, 10) })], rules(VIDEO), [everywhere]).result, 'pass');
});

test('an entry with a limit of its own tolerates a slowdown and not a stop', () => {
  // 30 frames a second, then 15 for three seconds; in the second recording 0.
  const slow = factor => {
    const rows = recording(10, { video: true, mount: true });
    let frames = 0;
    rows.forEach((row, second) => { frames += second >= 4 && second <= 6 ? 30 * factor : 30; row.browser.video.framesDecoded = frames; });
    return rows;
  };
  const criteria = rules({ while: { video: true }, measure: 'framesPerSecond', min: 27 });
  const entry = { issue: 'https://example.test/issues/2', while: { mount: true }, measure: 'framesPerSecond', min: 10, tolerateSeconds: 5 };
  assert.equal(judge(slow(0.5), criteria, [entry]).result, 'pass');
  const stopped = judge(slow(0), criteria, [entry]);
  assert.equal(stopped.result, 'fail');
  assert.deepEqual(stopped.failures[0].worseThan, [entry]);
});

test('a short known failure inside a long one has its own count of seconds', () => {
  // 20 frames a second for the whole recording, and a stop while the mount is on.
  const rows = stopSeconds => {
    const all = recording(40, second => ({ video: true, hid: true, mount: second >= 20 && second < 20 + stopSeconds }));
    let frames = 0;
    all.forEach(row => { frames += row.state.mount ? 0 : 20; row.browser.video.framesDecoded = frames; });
    return all;
  };
  const criteria = rules({ while: { video: true }, measure: 'framesPerSecond', min: 27 });
  const slow = { issue: 'https://example.test/issues/4', while: { hid: true }, measure: 'framesPerSecond', min: 15, tolerateSeconds: 60 };
  const stop = { issue: 'https://example.test/issues/5', while: { mount: true }, measure: 'framesPerSecond', tolerateSeconds: 5 };
  assert.equal(judge(rows(5), criteria, [slow, stop]).result, 'pass');
  assert.equal(judge(rows(7), criteria, [slow, stop]).result, 'fail');
});

test('an entry still applies for its recoverySeconds after its state has ended', () => {
  // Video is slow while the mount is on (seconds 5 to 14) and for 2 seconds after.
  const state = second => ({ video: true, mount: second >= 5 && second <= 14 });
  const entry = seconds => ({ issue: 'https://example.test/issues/4', while: { mount: true }, measure: 'framesPerSecond',
    tolerateSeconds: 16, recoverySeconds: seconds });
  const run = (frozen, seconds) => judge(recording(30, state, { frozen }), rules(VIDEO), [entry(seconds)]);
  assert.equal(run(range(5, 16), 2).result, 'pass');
  assert.equal(run(range(5, 16), 2).known[0].seconds, 12);
  assert.deepEqual(run(range(5, 16), 0).failures.map(f => f.t), [T0 + 15000]);
  assert.deepEqual(run(range(5, 17), 2).failures.map(f => f.t), [T0 + 17000]);
});

test('a breach that two entries apply to is known under both, and neither is noted as absent', () => {
  const stop = { issue: 'https://example.test/issues/5', while: { osVideo: true, mount: true }, measure: 'framesPerSecond', tolerateSeconds: 10 };
  const slow = { issue: 'https://example.test/issues/6', while: { osFiles: true, mount: true }, measure: 'framesPerSecond', tolerateSeconds: 16 };
  const { result, known, notOccurred } = judge(
    recording(30, { video: true, osVideo: true, osFiles: true, mount: true }, { frozen: range(5, 16) }), rules(VIDEO), [stop, slow]);
  assert.equal(result, 'pass');
  assert.deepEqual(known.map(k => k.entries), [[stop, slow]]);
  assert.deepEqual(notOccurred, []);
});

// A known fault: video stops while the update is on, and a mount beside OS
// Services video takes up to 14 s. Each is an entry with "fails".
const STOP_FAULT = { issue: 'https://example.test/issues/310', fails: true, while: { ota: true }, measure: 'framesPerSecond', tolerateSeconds: 6 };
const MOUNT_FAULT = { ...SLOW_MOUNT, issue: 'https://example.test/issues/319', fails: true, max: 14 };
const lastLine = verdict => report(verdict, T0).split('\n').at(-1);

test('a line that an entry with "fails" matches stays a failure, and the report names the issue beside it', () => {
  const stopped = judge(recording(30, { video: true, ota: true }, { frozen: range(5, 10) }), rules(VIDEO), [STOP_FAULT]);
  assert.equal(stopped.result, 'fail');
  assert.deepEqual(stopped.failures.map(f => [f.t, f.faults, f.worseThan]), [[T0 + 7000, [STOP_FAULT], []]]);
  assert.deepEqual([stopped.known, stopped.notOccurred], [[], []]);
  assert.equal(report(stopped, T0).split('\n')[1], 'FAIL  framesPerSecond is 0 for more than 2 s    known fault: #310');
  // The time a change takes.
  const slow = judge(mount(13, true), MOUNT_LIMIT, [MOUNT_FAULT]);
  assert.equal(slow.result, 'fail');
  assert.deepEqual(slow.failures.map(f => [f.faults, f.worseThan]), [[[MOUNT_FAULT], []]]);
  assert.equal(report(slow, T0).split('\n')[1], 'FAIL  mount on settled after 13.0 s, limit 10 s    known fault: #319');
  // The entry is matched like any other: not in another state, not on
  // another host, and not for a breach longer than its tolerateSeconds or
  // a change longer than its "max". Such a line has no issue beside it.
  const unnamed = verdict => verdict.failures.map(f => f.faults);
  assert.deepEqual(unnamed(judge(recording(30, { video: true }, { frozen: range(5, 10) }), rules(VIDEO), [STOP_FAULT])), [[]]);
  assert.deepEqual(unnamed(judge([{ t: T0, run: { host: 'linux' } }, ...recording(30, { video: true, ota: true }, { frozen: range(5, 10) })],
    rules(VIDEO), [{ ...STOP_FAULT, host: 'windows' }])), [[]]);
  const long = judge(recording(30, { video: true, ota: true }, { frozen: range(5, 11) }), rules(VIDEO), [STOP_FAULT]);
  assert.deepEqual(long.failures.map(f => [f.t, f.faults, f.worseThan]), [[T0 + 7000, [], [STOP_FAULT]]]);
  assert.doesNotMatch(report(long, T0), /known fault/);
  assert.deepEqual(judge(mount(15, true), MOUNT_LIMIT, [MOUNT_FAULT]).failures.map(f => [f.faults, f.worseThan]), [[[], [MOUNT_FAULT]]]);
  // With a limit of its own, the entry names a slowdown and not a stop.
  const slowOnly = { ...STOP_FAULT, min: 1 };
  assert.deepEqual(judge(recording(30, { video: true, ota: true }, { frozen: range(5, 10) }), rules(VIDEO), [slowOnly])
    .failures.map(f => [f.faults, f.worseThan]), [[[], [slowOnly]]]);
  // Where the entry applies and nothing breaks, it is a note, as any entry.
  assert.deepEqual(judge(recording(30, { video: true, ota: true }), rules(VIDEO), [STOP_FAULT]).notOccurred, [STOP_FAULT]);
});

test('a run ends in one of three ways: it passes, it fails with known faults only, or it fails with a line that no entry names', () => {
  const run = (state, frozen, seconds) => judge([...recording(30, state, { frozen }), ...mount(seconds, true)],
    { ...rules(VIDEO), ...MOUNT_LIMIT }, [STOP_FAULT, MOUNT_FAULT]);
  const passed = run({ video: true, ota: true }, [], 4);
  assert.deepEqual([passed.result, passed.outcome, lastLine(passed), EXIT[passed.outcome]], ['pass', 'pass', 'PASS', 0]);
  // Both failures have an entry with "fails".
  const faults = run({ video: true, ota: true }, range(5, 10), 13);
  assert.deepEqual([faults.result, faults.outcome, faults.unnamed], ['fail', 'known faults', 0]);
  assert.equal(lastLine(faults), 'FAIL  known faults only: #310, #319');
  assert.equal(EXIT[faults.outcome], 3);
  // The same run with a mount that takes longer than the fault's 14 s.
  const more = run({ video: true, ota: true }, range(5, 10), 15);
  assert.deepEqual([more.result, more.outcome, more.unnamed], ['fail', 'fail', 1]);
  assert.equal(lastLine(more), 'FAIL  1 line that no entry names, and known faults: #310');
  assert.equal(EXIT[more.outcome], 1);
  // No known fault at all.
  const plain = run({ video: true }, range(5, 10), 15);
  assert.equal(lastLine(plain), 'FAIL  2 lines that no entry names');
  // A sample without a verdict beside known faults is a line that no entry names.
  const tie = judge(recording(30, { video: true, ota: true, mount: true }, { frozen: range(5, 10) }),
    rules(VIDEO, { while: { video: true, mount: true }, measure: 'mountReadBytesPerSecond', min: 1 },
      { while: { ota: true, mount: true }, measure: 'mountReadBytesPerSecond', min: 2 }), [STOP_FAULT]);
  assert.deepEqual([tie.failures.map(f => f.faults), tie.undecided.length, tie.outcome], [[[STOP_FAULT]], 1, 'fail']);
  assert.equal(lastLine(tie), 'FAIL  1 line that no entry names, and known faults: #310');
});

test('an entry without "fails" that covers a line makes it known; an entry with "fails" names the line only where no such entry covers it', () => {
  // Video is known to stop for up to 3 s during an update. A stop of up to
  // 6 s is the known fault.
  const short = { issue: 'https://example.test/issues/2', while: { ota: true }, measure: 'framesPerSecond', tolerateSeconds: 3 };
  for (const known of [[short, STOP_FAULT], [STOP_FAULT, short]]) {
    const run = frozen => judge(recording(30, { video: true, ota: true }, { frozen }), rules(VIDEO), known);
    const covered = run(range(5, 7));
    assert.deepEqual([covered.outcome, covered.failures, covered.notOccurred], ['pass', [], []]);
    assert.deepEqual(covered.known.map(k => [k.entries, k.seconds]), [[[short], 3]]);
    // One second more than the entry without "fails" tolerates.
    const fault = run(range(5, 8));
    assert.deepEqual([fault.outcome, fault.known], ['known faults', []]);
    assert.deepEqual(fault.failures.map(f => [f.t, f.faults, f.worseThan]), [[T0 + 8000, [STOP_FAULT], [short]]]);
    // One second more than the entry with "fails" tolerates: the failure
    // that had the issue beside it loses it.
    const worse = run(range(5, 11));
    assert.deepEqual([worse.outcome, worse.unnamed], ['fail', 1]);
    assert.deepEqual(worse.failures.map(f => [f.t, f.faults, new Set(f.worseThan)]), [[T0 + 8000, [], new Set([short, STOP_FAULT])]]);
  }
  // The time a change takes: SLOW_MOUNT covers up to 12 s, the fault up to 14 s.
  const took = seconds => judge(mount(seconds, true), MOUNT_LIMIT, [MOUNT_FAULT, SLOW_MOUNT]);
  assert.deepEqual([took(11).outcome, took(11).known.map(k => k.entries), took(11).notOccurred], ['pass', [[SLOW_MOUNT]], []]);
  assert.deepEqual([took(13).outcome, took(13).failures.map(f => [f.faults, f.worseThan])], ['known faults', [[[MOUNT_FAULT], [SLOW_MOUNT]]]]);
  assert.deepEqual([took(15).outcome, took(15).failures[0].faults], ['fail', []]);
});

test('a failure that goes on into the state of a second entry with "fails" has both issues, and neither entry is noted as absent', () => {
  // Video stops in second 5, during the update, and stays stopped for 5 s
  // after the update has ended and a mount has come on.
  const state = second => (second < 10 ? { video: true, ota: true } : { video: true, mount: true });
  const mountFault = { issue: 'https://example.test/issues/311', fails: true, while: { mount: true }, measure: 'framesPerSecond', tolerateSeconds: 6 };
  const run = known => judge(recording(30, state, { frozen: range(5, 14) }), rules(VIDEO), known);
  const both = run([STOP_FAULT, mountFault]);
  assert.deepEqual(both.failures.map(f => [f.t, f.faults, f.worseThan]), [[T0 + 7000, [STOP_FAULT, mountFault], []]]);
  assert.deepEqual([both.outcome, both.notOccurred], ['known faults', []]);
  assert.equal(lastLine(both), 'FAIL  known faults only: #310, #311');
  // Without the second entry, nothing names the seconds after the update.
  assert.deepEqual(run([STOP_FAULT]).failures.map(f => [f.faults, f.worseThan]), [[[], [STOP_FAULT]]]);
});

test('judge.mjs exits with 3 for a recording that fails with known faults only, and with 1 when a line has no entry', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-faults-'));
  try {
    const file = (name, text) => { fs.writeFileSync(path.join(directory, name), text); return path.join(directory, name); };
    const rows = recording(30, { video: true, ota: true }, { frozen: range(5, 10) });
    const args = [path.join(here, 'judge.mjs'), file('recording.jsonl', rows.map(row => JSON.stringify(row)).join('\n')),
      file('criteria.jsonc', JSON.stringify(rules(VIDEO)))];
    const exit = known => spawnSync(process.execPath, [...args, file('known.jsonc', JSON.stringify(known))], { encoding: 'utf8' });
    const faults = exit([STOP_FAULT]);
    assert.equal(faults.status, 3, faults.stderr);
    assert.equal(faults.stdout.trimEnd().split('\n').at(-1), 'FAIL  known faults only: #310');
    assert.equal(exit([]).status, 1);
    assert.equal(exit([{ ...STOP_FAULT, fails: undefined }]).status, 0);
  } finally {
    fs.rmSync(directory, { recursive: true });
  }
});

// A mount whose switch was not done when the walk moved on, 12 s after its
// start: the walk's limit was 7 s, and it waited 5 s more.
const unfinishedMount = (state = { video: true, osServices: true, httpMountCdrom: true }) => [
  ...recording(30, { video: true, osServices: true }),
  { t: T0 + 14_000, change: { httpMountCdrom: true }, state, startedAt: T0 + 2000, seconds: 12, error: null, unfinished: 'httpMountCdrom is still off' }];
const mountLimit = max => ({ changeTakesAtMost: [{ change: { httpMountCdrom: true }, max }] });
// The mount beside OS Services is known to take up to 20 s.
const SLOWER_MOUNT = { issue: 'https://example.test/issues/2', change: { httpMountCdrom: true }, while: { osServices: true }, max: 20 };

test('judged again, an unfinished change whose limit is now larger than the walk waited asks for a new walk', () => {
  const again = (max, known = []) => judge(unfinishedMount(), mountLimit(max), known, { again: true });
  const longer = again(13);
  assert.deepEqual([longer.failures, longer.walkAgain.map(w => [w.change.change, w.limit, w.movedOn - T0])],
    [[], [[{ httpMountCdrom: true }, 13, 14_000]]]);
  assert.match(report(longer, T0), /^WALK AGAIN {2}httpMountCdrom on was not finished after 12\.0 s, and the current limit is 13 s; the walk moved on at 14 s$/m);
  // A known failure's larger limit for the change counts as well, in its state only.
  assert.equal(again(7, [SLOWER_MOUNT]).walkAgain.length, 1);
  assert.equal(judge(unfinishedMount({ video: true, httpMountCdrom: true }), mountLimit(7), [SLOWER_MOUNT], { again: true }).failures.length, 1);
  // The same limit or a smaller one: the walk waited long enough, and the change fails.
  for (const max of [12, 7]) {
    const verdict = again(max);
    assert.deepEqual([verdict.walkAgain, verdict.failures.map(f => f.unfinished)], [[], ['httpMountCdrom is still off']]);
  }
  // A live walk has no WALK AGAIN.
  assert.deepEqual([judge(unfinishedMount(), mountLimit(13)).walkAgain, judge(unfinishedMount(), mountLimit(13)).failures.length], [[], 1]);
});

test('a run with a change to walk again and no line that fails without an entry ends as "walk again", with exit status 4', () => {
  const walkAgain = judge(unfinishedMount(), mountLimit(13), [], { again: true });
  assert.deepEqual([walkAgain.result, walkAgain.outcome, EXIT[walkAgain.outcome]], ['pass', 'walk again', 4]);
  assert.equal(lastLine(walkAgain), 'WALK AGAIN  1 change that the recording cannot judge');
  // Beside known faults only.
  const rows = [...recording(30, { video: true, ota: true }, { frozen: range(5, 10) }), unfinishedMount().at(-1)];
  const faults = judge(rows, { ...rules(VIDEO), ...mountLimit(13) }, [STOP_FAULT], { again: true });
  assert.deepEqual([faults.outcome, lastLine(faults)], ['walk again', 'WALK AGAIN  1 change that the recording cannot judge, and known faults: #310']);
  // Beside a line that no entry names, the run fails.
  const failed = judge(rows, { ...rules(VIDEO), ...mountLimit(13) }, [], { again: true });
  assert.deepEqual([failed.outcome, failed.walkAgain.length, lastLine(failed)], ['fail', 1, 'FAIL  1 line that no entry names']);
});

test('judge.mjs exits with 4 for a change to walk again, and with 2 for criteria or known failures that it cannot judge with', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-again-'));
  try {
    const file = (name, content) => { fs.writeFileSync(path.join(directory, name), JSON.stringify(content)); return path.join(directory, name); };
    const recordingFile = path.join(directory, 'recording.jsonl');
    fs.writeFileSync(recordingFile, unfinishedMount().map(row => JSON.stringify(row)).join('\n'));
    const exit = (criteria, known) => spawnSync(process.execPath,
      [path.join(here, 'judge.mjs'), recordingFile, file('criteria.jsonc', criteria), file('known.jsonc', known)], { encoding: 'utf8' });
    const again = exit(mountLimit(13), []);
    assert.equal(again.status, 4, again.stderr);
    assert.match(again.stdout, /^WALK AGAIN {2}httpMountCdrom on was not finished after 12\.0 s/m);
    const badRule = exit({ everySecond: [{ while: { nothing: true }, measure: 'framesPerSecond', min: 1 }] }, []);
    assert.deepEqual([badRule.status, badRule.stdout], [2, '']);
    assert.match(badRule.stderr, /criteria\.jsonc: .*nothing/);
    const badEntry = exit(mountLimit(13), [{ ...SLOWER_MOUNT, while: { nothing: true } }]);
    assert.deepEqual([badEntry.status, badEntry.stdout], [2, '']);
    assert.match(badEntry.stderr, /known\.jsonc: .*nothing/);
  } finally {
    fs.rmSync(directory, { recursive: true });
  }
});

test('the tool\'s version is the commit of the checkout and whether tools/combined_load has changes that are not committed', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-version-'));
  try {
    const git = (...args) => spawnSync('git', ['-C', directory, ...args], { encoding: 'utf8' }).stdout.trim();
    const write = (name, text) => {
      fs.mkdirSync(path.dirname(path.join(directory, name)), { recursive: true });
      fs.writeFileSync(path.join(directory, name), text);
    };
    git('init', '-q');
    write('tools/combined_load/lists.jsonc', '{}');
    write('README.md', 'a');
    git('add', '.');
    git('-c', 'user.name=t', '-c', 'user.email=t@example.test', 'commit', '-q', '-m', 'one');
    const commit = git('rev-parse', 'HEAD');
    assert.match(commit, /^[0-9a-f]{40}$/);
    assert.deepEqual(toolVersion(directory), { commit, changed: false });
    // A change outside the tool is not the tool's.
    write('README.md', 'b');
    assert.deepEqual(toolVersion(directory), { commit, changed: false });
    // A file that git does not track is a change of the tool, as is a change of a committed file.
    write('tools/combined_load/new.mjs', '');
    assert.deepEqual(toolVersion(directory), { commit, changed: true });
    fs.rmSync(path.join(directory, 'tools/combined_load/new.mjs'));
    write('tools/combined_load/lists.jsonc', '{"a":[]}');
    assert.deepEqual(toolVersion(directory), { commit, changed: true });
    // Outside a checkout git cannot tell.
    fs.rmSync(path.join(directory, '.git'), { recursive: true });
    const outside = toolVersion(directory);
    assert.deepEqual([Object.keys(outside), typeof outside.error], [['error'], 'string']);
  } finally {
    fs.rmSync(directory, { recursive: true });
  }
});

test('a judge of a saved recording notes a tool that can differ from its own, and its verdict stays the same', () => {
  const A = 'a'.repeat(40);
  const B = 'b'.repeat(40);
  assert.deepEqual(toolVersionNotes({ commit: A, changed: false }, { commit: A, changed: false }), []);
  assert.deepEqual(toolVersionNotes({ commit: A, changed: false }, { commit: B, changed: false }),
    [`the recording was made by the tool at ${A}, and the judge runs at ${B}`]);
  // The same commit with changes on either side.
  assert.deepEqual(toolVersionNotes({ commit: A, changed: true }, { commit: A, changed: false }),
    [`the recording was made by the tool at ${A} with changes in tools/combined_load that are not committed, and the judge runs at ${A}`]);
  assert.deepEqual(toolVersionNotes({ commit: A, changed: false }, { commit: A, changed: true }),
    [`the recording was made by the tool at ${A}, and the judge runs at ${A} with changes in tools/combined_load that are not committed`]);
  // Two versions that git could not tell are not the same.
  assert.deepEqual(toolVersionNotes({ error: 'no git' }, { error: 'no git' }),
    ['the recording was made by the tool at an unknown commit (no git), and the judge runs at an unknown commit (no git)']);
  assert.deepEqual(toolVersionNotes(undefined, { commit: A, changed: false }), ['the recording does not name its tool: its first row has no "tool"']);

  // judge.mjs prints the note for the recording's tool against its own checkout; the rest of the report and the exit status stay.
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-tool-'));
  try {
    const current = toolVersion(path.resolve(here, '..', '..', '..'));
    assert.match(current.commit, /^[0-9a-f]{40}$/);
    const criteria = path.join(directory, 'criteria.jsonc');
    fs.writeFileSync(criteria, JSON.stringify(rules(VIDEO)));
    const judged = tool => {
      const file = path.join(directory, 'recording.jsonl');
      const first = { t: T0, run: { board: 'ethernet', ...(tool && { tool }) } };
      fs.writeFileSync(file, [first, ...recording(30, { video: true }, { frozen: range(5, 10) })].map(row => JSON.stringify(row)).join('\n'));
      const result = spawnSync(process.execPath, [path.join(here, 'judge.mjs'), file, criteria, path.join(directory, 'none.jsonc')], { encoding: 'utf8' });
      const lines = result.stdout.trimEnd().split('\n');
      const note = line => /^NOTE {2}the recording (was made by the|does not name its) tool/.test(line);
      return { status: result.status, notes: lines.filter(note), rest: lines.filter(line => !note(line)) };
    };
    fs.writeFileSync(path.join(directory, 'none.jsonc'), '[]');
    const same = judged(current);
    assert.equal(same.status, 1);
    assert.deepEqual(same.rest.at(-1), 'FAIL  1 line that no entry names');
    // A checkout with changes in the tool notes them even beside its own commit.
    assert.deepEqual(same.notes, current.changed
      ? [`NOTE  the recording was made by the tool at ${current.commit} with changes in tools/combined_load that are not committed, and the judge runs at ${current.commit} with changes in tools/combined_load that are not committed`]
      : []);
    const other = judged({ commit: A, changed: false });
    assert.deepEqual(other.notes, [`NOTE  the recording was made by the tool at ${A}, and the judge runs at ${current.commit}${current.changed ? ' with changes in tools/combined_load that are not committed' : ''}`]);
    const unnamed = judged(undefined);
    assert.deepEqual(unnamed.notes, ['NOTE  the recording does not name its tool: its first row has no "tool"']);
    for (const result of [other, unnamed]) assert.deepEqual([result.status, result.rest], [same.status, same.rest]);
  } finally {
    fs.rmSync(directory, { recursive: true });
  }
  // A walk judges its own recording without the note.
  const rows = [{ t: T0, run: { board: 'ethernet' } }, ...recording(10, { video: true })];
  assert.ok(!/NOTE/.test(report(judge(rows, rules(VIDEO)), T0)));
});

test('a rule whose state came up and whose measure has no value in the whole recording has a note', () => {
  const noKeys = { while: { video: true }, measure: 'keysLost', max: 0 };
  const otherState = { while: { hid: true }, measure: 'keysLost', max: 0 };
  const verdict = judge(recording(10, { video: true }), rules(VIDEO, noKeys, otherState));
  assert.deepEqual(verdict.noData, [noKeys]);
  assert.match(report(verdict, T0), /^NOTE {2}the recording has no data for the rule of keysLost\n {6}\{"while":\{"video":true\},"measure":"keysLost","max":0\}$/m);
  // A value in any sample of the recording is data for the rule.
  const someKeys = recording(10, second => (second < 5 ? { video: true } : { video: true, hid: true }))
    .map(row => (row.state.hid ? { ...row, loads: { hid: { sends: [], keys: [], moves: [], reports: [] } } } : row));
  assert.deepEqual(judge(someKeys, rules(VIDEO, noKeys)).noData, []);
});

// A breach that a change causes. The cloud comes on from second 5 to second
// 7; the entry applies in the samples of seconds 6 and 7 and, with 2 seconds
// to recover, 8 and 9.
const EVERY_FRAME = { while: { video: true }, measure: 'framesPerSecond', min: 1 };
const AT_CLOUD_ON = { issue: 'https://example.test/issues/7', change: { cloud: true }, measure: 'framesPerSecond',
  tolerateSeconds: 4, recoverySeconds: 2 };
const cloudOn = (frozen, state = {}) => [change('cloud', true, 5, 2, { state: { video: true, ...state } }),
  ...recording(20, second => ({ video: true, ...state, cloud: second >= 5 }), { frozen })];

test('an entry for a measure during a change covers a breach in the change and in its recoverySeconds', () => {
  const run = (frozen, entry = AT_CLOUD_ON) => judge(cloudOn(frozen), rules(EVERY_FRAME), [entry]);
  const covered = run(range(6, 9));
  assert.equal(covered.result, 'pass');
  assert.deepEqual(covered.known.map(k => [k.entries, k.seconds]), [[[AT_CLOUD_ON], 4]]);
  assert.deepEqual(covered.notOccurred, []);
  // The second before the change, and the second after the recovery.
  assert.deepEqual(run([5]).failures.map(f => [f.t, f.worseThan]), [[T0 + 5000, []]]);
  assert.deepEqual(run([10]).failures.map(f => [f.t, f.worseThan]), [[T0 + 10000, []]]);
  // Without recoverySeconds the entry ends with the change.
  assert.deepEqual(run([8], { ...AT_CLOUD_ON, recoverySeconds: undefined }).failures.map(f => f.t), [T0 + 8000]);
  // A breach that goes on after the recovery fails there, as worse than the entry.
  assert.deepEqual(run(range(6, 10)).failures.map(f => [f.t, f.worseThan]), [[T0 + 10000, [AT_CLOUD_ON]]]);
  // The change without the breach is a note.
  assert.equal(run([]).result, 'pass');
  assert.deepEqual(run([]).notOccurred, [AT_CLOUD_ON]);
});

test('an entry for a measure during a change has its tolerateSeconds, its own limit and the state of the change', () => {
  const short = { ...AT_CLOUD_ON, tolerateSeconds: 3 };
  assert.deepEqual(judge(cloudOn(range(6, 9)), rules(EVERY_FRAME), [short]).failures.map(f => [f.t, f.worseThan]), [[T0 + 9000, [short]]]);
  // The entry counts the seconds of the breach from its first one, also those
  // that the rule's failAfterSeconds allows: a rule that fails in the fourth
  // second needs an entry that tolerates four.
  const afterThree = rules({ ...EVERY_FRAME, failAfterSeconds: 3 });
  assert.equal(judge(cloudOn(range(6, 9)), afterThree, [AT_CLOUD_ON]).result, 'pass');
  assert.deepEqual(judge(cloudOn(range(6, 9)), afterThree, [short]).failures.map(f => [f.t, f.worseThan]), [[T0 + 9000, [short]]]);

  // 30 frames a second, and 15 or 0 in second 6.
  const slow = factor => {
    const rows = cloudOn([]);
    let frames = 0;
    rows.filter(row => row.browser).forEach((row, second) => { frames += second === 6 ? 30 * factor : 30; row.browser.video.framesDecoded = frames; });
    return rows;
  };
  const criteria = rules({ while: { video: true }, measure: 'framesPerSecond', min: 27 });
  const limited = { ...AT_CLOUD_ON, min: 10 };
  assert.equal(judge(slow(0.5), criteria, [limited]).result, 'pass');
  assert.deepEqual(judge(slow(0), criteria, [limited]).failures.map(f => f.worseThan), [[limited]]);

  // "while" names what is on when the change takes place.
  const besideKeys = { ...AT_CLOUD_ON, while: { hid: true } };
  assert.equal(judge(cloudOn([6], { hid: true }), rules(EVERY_FRAME), [besideKeys]).result, 'pass');
  const without = judge(cloudOn([6]), rules(EVERY_FRAME), [besideKeys]);
  assert.deepEqual(without.failures.map(f => f.worseThan), [[]]);
  assert.deepEqual(without.notOccurred, []);
  // Another change, and the same variable going off, are not that change.
  const off = [change('cloud', false, 5, 2, { state: { video: true } }), ...recording(20, { video: true }, { frozen: [6] })];
  assert.equal(judge(off, rules(EVERY_FRAME), [AT_CLOUD_ON]).result, 'fail');
});

test('a breach during a change is known under the entry for the change and the entry for the state', () => {
  const state = { issue: 'https://example.test/issues/8', while: { mount: true }, measure: 'framesPerSecond', tolerateSeconds: 10 };
  const { result, known, notOccurred } = judge(cloudOn(range(6, 7), { mount: true }), rules(EVERY_FRAME), [AT_CLOUD_ON, state]);
  assert.equal(result, 'pass');
  assert.deepEqual(known.map(k => k.entries), [[AT_CLOUD_ON, state]]);
  assert.deepEqual(notOccurred, []);
  // The state's entry alone covers what goes on after the change's recovery.
  const long = judge(cloudOn(range(6, 12), { mount: true }), rules(EVERY_FRAME), [AT_CLOUD_ON, state]);
  assert.equal(long.result, 'pass');
  assert.deepEqual(long.known.map(k => [k.entries, k.seconds]), [[[AT_CLOUD_ON, state], 7]]);
});

test('an entry for a measure during a change applies after the change\'s own pause of that measure', () => {
  const criteria = { everySecond: [EVERY_FRAME], changeTakesAtMost: [{ change: { cloud: true }, max: 15, pauses: ['framesPerSecond'] }] };
  // No frames in the change, which the pause takes, and in the 2 seconds after it.
  const paused = judge(cloudOn(range(6, 9)), criteria, [AT_CLOUD_ON]);
  assert.equal(paused.result, 'pass');
  assert.deepEqual(paused.known.map(k => [k.t, k.seconds]), [[T0 + 8000, 2]]);
  assert.deepEqual(judge(cloudOn(range(6, 9)), criteria, []).failures.map(f => f.t), [T0 + 8000]);
  assert.deepEqual(judge(cloudOn(range(6, 10)), criteria, [AT_CLOUD_ON]).failures.map(f => f.t), [T0 + 10000]);
});

test('an entry for a measure during a change says nothing about how long the change takes', () => {
  const criteria = { everySecond: [EVERY_FRAME], changeTakesAtMost: [{ change: { cloud: true }, max: 1 }] };
  const { failures, known } = judge(cloudOn([6]), criteria, [AT_CLOUD_ON]);
  assert.deepEqual(failures.map(f => [f.change?.change, f.worseThan]), [[{ cloud: true }, []]]);
  assert.deepEqual(known.map(k => k.measure), ['framesPerSecond']);
});

test('"andOneOf" and a list of measures make one rule or entry each', () => {
  const entry = { issue: 'https://example.test/issues/3', while: { video: true }, andOneOf: ['mount', 'upload'],
    measure: ['framesPerSecond', 'freezeMs'], tolerateSeconds: 5 };
  assert.deepEqual(expand([entry]).map(e => [Object.keys(e.while).join('+'), e.measure, e.tolerateSeconds]), [
    ['video+mount', 'framesPerSecond', 5], ['video+mount', 'freezeMs', 5],
    ['video+upload', 'framesPerSecond', 5], ['video+upload', 'freezeMs', 5],
  ]);
  // The entry covers a breach with the mount on, and not one without it.
  const criteria = rules(VIDEO);
  assert.equal(judge(recording(10, { video: true, mount: true }, { frozen: range(3, 6) }), criteria, [entry]).result, 'pass');
  assert.equal(judge(recording(10, { video: true }, { frozen: range(3, 6) }), criteria, [entry]).result, 'fail');
  assert.throws(() => validateKnown([{ ...entry, andOneOf: ['nothing'] }], ['video', 'mount', 'upload']), /unknown variable "nothing"/);
});

test('the rules are checked before a run', () => {
  const rejects = criteria => assert.throws(() => validateCriteria(criteria, ['video', 'mount']));
  rejects(rules({ measure: 'framesPerSec', min: 1 }));
  rejects(rules({ while: { vidoe: true }, measure: 'framesPerSecond', min: 1 }));
  rejects(rules({ while: { video: true }, measure: 'framesPerSecond' }));
  rejects(rules({ while: { video: true }, measure: 'framesPerSecond', min: 1, allow: 2 }));
  rejects({ changeTakesAtMost: [{ change: { mout: true }, max: 5 }] });
  rejects({ changeTakesAtMost: [{ change: { mount: true } }] });
  rejects({ changeTakesAtMost: [{ change: { mount: 'on' }, max: 5 }] });
  rejects({ changeTakesAtMost: [{ change: { mount: true }, max: 5, pauses: ['framesPerSec'] }] });
  rejects({ changeTakesAtMost: [{ change: { mount: true }, max: 5, pauses: { framesPerSecond: 4 } }] });
  rejects({ changes: [] });
});

test('the known failures are checked before a run', () => {
  const rejects = entry => assert.throws(() => validateKnown([entry], ['video', 'mount', 'osServices']));
  rejects({ change: { mount: true }, max: 12 });
  rejects({ issue: 'u', change: { mount: true } });
  rejects({ issue: 'u', measure: 'framesPerSecond', tolerateSeconds: 8 });
  rejects({ issue: 'u', while: { osService: true }, measure: 'framesPerSecond', tolerateSeconds: 8 });
  rejects({ issue: 'u', while: { osServices: true }, measure: 'framesPerSecond' });
  // The two forms with "change": the time the change takes, and a measure during it.
  const message = entry => { try { validateKnown([entry], ['video', 'mount']); } catch (error) { return error.message; } return null; };
  assert.equal(message({ issue: 'u', change: { mount: true }, max: 12 }), null);
  assert.equal(message({ issue: 'u', change: { mount: true }, measure: 'framesPerSecond', min: 10, tolerateSeconds: 2, recoverySeconds: 1 }), null);
  assert.equal(message({ issue: 'u', change: { mount: true }, while: { video: true }, measure: ['framesPerSecond', 'freezeMs'], tolerateSeconds: 2 }), null);
  assert.match(message({ issue: 'u', change: { mount: true }, min: 3, max: 12 }), /no "measure" is for the time the change takes/);
  assert.match(message({ issue: 'u', change: { mount: true }, max: 12, tolerateSeconds: 2 }), /"tolerateSeconds" and "recoverySeconds" belong to an entry with "measure"/);
  assert.match(message({ issue: 'u', change: { mount: true }, measure: 'framesPerSecond', max: 12 }), /"change" and "measure" is for that measure during the change: no tolerateSeconds/);
  assert.match(message({ issue: 'u', change: { mount: true }, measure: 'framesPerSec', tolerateSeconds: 2 }), /during the change: unknown measure "framesPerSec"/);
  // A host is one of the three names the tool gives a host; a rule has none.
  const onHost = host => [{ issue: 'u', host, change: { osServices: false }, max: 16 }];
  for (const host of ['linux', 'macos', 'windows']) validateKnown(onHost(host), ['osServices']);
  for (const host of ['Windows', 'win', 'MINGW64_NT-10.0-26300', '', null, ['windows']]) {
    assert.throws(() => validateKnown(onHost(host), ['osServices']), /"host" is one of linux, macos, windows/);
  }
  assert.throws(() => validateCriteria({ everySecond: [{ host: 'windows', while: { video: true }, measure: 'framesPerSecond', min: 1 }] }, ['video']),
    /unknown key "host"/);
  // "fails" is true or absent, in both forms of an entry; a rule has none.
  validateKnown([{ issue: 'u', fails: true, change: { osServices: false }, max: 16 }], ['osServices']);
  validateKnown([{ issue: 'u', fails: true, while: { video: true }, measure: 'framesPerSecond', tolerateSeconds: 2 }], ['video']);
  for (const fails of [false, 'true', 1, null, 'https://example.test/issues/1']) {
    assert.throws(() => validateKnown([{ issue: 'u', fails, change: { osServices: false }, max: 16 }], ['osServices']), /"fails" is true, or the entry has no "fails"/);
  }
  assert.throws(() => validateCriteria({ everySecond: [{ fails: true, while: { video: true }, measure: 'framesPerSecond', min: 1 }] }, ['video']),
    /unknown key "fails"/);
  assert.throws(() => validateCriteria({ changeTakesAtMost: [{ fails: true, change: { video: true }, max: 5 }] }, ['video']), /unknown key "fails"/);
});

test('the rules, known failures and lists in this directory are valid', () => {
  for (const board of ['ethernet', 'wifi']) {
    validateCriteria(readJsonc(path.join(here, '..', board, 'criteria.jsonc')), VARIABLES);
    validateKnown(readJsonc(path.join(here, '..', board, 'known-failures.jsonc')), VARIABLES);
  }
  for (const list of Object.values(readJsonc(path.join(here, '..', 'lists.jsonc')))) validateList(list, VARIABLES);
});

test('the walk has every two things a user can have at once together, and with each video source', () => {
  const states = readJsonc(path.join(here, '..', 'lists.jsonc')).walk.map(step => step.state);
  const together = (...names) => states.some(state => names.every(name => state[name]));
  const beside = ['cloud', 'osTerminal', 'osFiles', 'osClipboard', 'hid'];
  const mounts = ['httpMountCdrom', 'httpMountDisk', 'tfMountCdrom', 'tfMountDisk'];
  const missing = [];
  for (const a of beside) {
    for (const b of [...beside, ...mounts]) if (a !== b && !together(a, b)) missing.push(`${a} + ${b}`);
    // Beside service video and beside HDMI capture.
    if (!together(a, 'osVideo')) missing.push(`${a} + osVideo`);
    if (!states.some(state => state[a] && !state.osVideo)) missing.push(`${a} without osVideo`);
  }
  // Each source and each kind of drive beside each video source.
  for (const part of ['httpMount', 'tfMount', 'Cdrom', 'Disk']) {
    const kind = mounts.filter(name => name.includes(part));
    if (!states.some(state => state.osVideo && kind.some(name => state[name]))) missing.push(`${part} + osVideo`);
    if (!states.some(state => !state.osVideo && kind.some(name => state[name]))) missing.push(`${part} without osVideo`);
  }
  if (!together('upload', 'osVideo')) missing.push('upload + osVideo');
  if (!states.some(state => state.upload && !state.osVideo)) missing.push('upload without osVideo');
  // The mount beside an update is the disk from an HTTPS URL.
  if (!states.some(state => state.ota && state.cloud && [...mounts, 'httpsMountDisk'].some(name => state[name]))) missing.push('ota + cloud + a mount');
  // The host's sound: beside video alone, beside an upload, beside service
  // video, and beside the session with its loads and keys.
  if (!states.some(state => state.audio && Object.keys(state).filter(name => state[name]).length === 2)) missing.push('audio beside video alone');
  for (const other of ['upload', 'osVideo', ...beside]) if (!together('audio', other)) missing.push(`audio + ${other}`);
  assert.deepEqual(missing, []);
  // MQTT is on from the first upload to the state with the HTTPS disk and
  // the update checks, and from the first update to the end.
  const firstUpload = states.findIndex(state => state.upload);
  const checks = states.findIndex(state => state.httpsMountDisk && state.updateCheck);
  const firstUpdate = states.findIndex(state => state.ota);
  assert.deepEqual(states.map(state => Boolean(state.mqtt)),
    states.map((state, index) => (index >= firstUpload && index <= checks) || index >= firstUpdate));
  // The cloud comes on in a step of its own. The browser leaves the device
  // page for the adoption, so nothing that the page's session carries is on
  // in that step; the sender's session with the Mini is. The session's loads
  // and the keys come on again in the step after.
  // What is on in a state beside MQTT, which every state has.
  const on = state => Object.keys(state).filter(name => state[name] && name !== 'mqtt').sort();
  const first = states.findIndex(state => state.cloud && state.osServices);
  assert.deepEqual(on(states[first]), ['cloud', 'osServices', 'video']);
  assert.deepEqual(beside.filter(name => !states[first + 1][name]), []);
  // No state after it turns the cloud off.
  assert.deepEqual(states.slice(first).filter(state => !state.cloud), []);
  // One adoption comes before it, without a session of the sender: the cloud's socket beside the HTTPS image and the update checks.
  assert.deepEqual(states.slice(0, first).filter(state => state.cloud).map(on), [['cloud', 'httpsMountDisk', 'updateCheck', 'video']]);
  // The loss comes once, beside the session's loads and the keys and
  // nothing else, and the walk has that state without the loss as well.
  const loaded = ['cloud', 'hid', 'osClipboard', 'osFiles', 'osServices', 'osTerminal', 'video'];
  assert.deepEqual(states.filter(state => state.packetLoss).map(on), [[...loaded, 'packetLoss'].sort()]);
  assert.ok(states.some(state => on(state).join() === loaded.join()));
  // The light terminal, the sound and the loss beside the session follow
  // the adoption, after the session's own state and before the first mount
  // that follows it.
  const firstMount = states.findIndex((state, index) => index > first && mounts.some(name => state[name]));
  assert.deepEqual(states.slice(first + 2, firstMount).map(on), [
    ['cloud', 'hid', 'osClipboard', 'osFiles', 'osServices', 'osTerminalLight', 'video'],
    [...loaded, 'audio'].sort(),
    [...loaded, 'packetLoss'].sort()]);
  // The takeover comes once, beside video and the sound alone: it ends the
  // first tab's session, so nothing that the session carries is on.
  assert.deepEqual(states.filter(state => state.takeover).map(on), [['audio', 'takeover', 'video']]);
  // The host changes its display mode once, during an upload that runs
  // already, with HDMI capture as the source. The upload ends in the other
  // mode, and the host changes back beside video alone.
  const mode = states.findIndex(state => state.hostModeChange);
  assert.deepEqual(states.slice(mode - 1, mode + 2).map(on), [['upload', 'video'], ['hostModeChange', 'upload', 'video'], ['hostModeChange', 'video']]);
  assert.equal(states.filter(state => state.hostModeChange).length, 2);
  // No state has only what both its neighbours have: the driver goes from
  // one loaded state to the next, and settling stands between them. The two
  // states with video alone around the series of update checks are an
  // exception: the Mini's memory is compared in them. The other one is the
  // state of the small drops without service video, between two with it:
  // it is there for the two changes, in which service video ends and comes on.
  const within = (inner, outer) => on(inner).every(name => outer[name]) && on(inner).length < on(outer).length;
  assert.deepEqual(states.filter((state, index) => index > 0 && index < states.length - 1 &&
    within(state, states[index - 1]) && within(state, states[index + 1])).map(on),
  [['video'], ['video'], ['osClipboard', 'osFilesSmall', 'osServices', 'video']]);
  // The slow keys come on once, beside video alone: nothing else of the page sends a message between two keys.
  const slow = states.filter(state => state.slowKeys);
  assert.deepEqual(slow.map(on), [['slowKeys', 'video']]);
  // What a user cannot have at once.
  for (const state of states) {
    const on = Object.keys(state).filter(name => state[name]);
    if (state.upload) {
      // The host's display mode is changed on the host, not in the Mini's page.
      assert.deepEqual(on.filter(name => !['video', 'mqtt', 'upload', 'osServices', 'osVideo', 'audio', 'hostModeChange'].includes(name)), []);
    }
    // A mount and an update take the Mini's audio output away; the tool has the sound off for them.
    if (state.audio) assert.deepEqual(on.filter(name => mounts.includes(name) || name === 'ota'), []);
    // During an update the Mini sends no video: no keys and no clipboard beside it.
    if (state.ota) assert.deepEqual(on.filter(name => ['osClipboard', 'hid'].includes(name)), []);
    assert.ok(mounts.filter(name => state[name]).length <= 1, 'one mount at a time');
  }
  // No upload beside an update: the Mini refuses one while an update
  // installs.
  assert.deepEqual(states.filter(state => state.upload && state.ota), []);
  const at = (...names) => states.findIndex(state => names.every(name => state[name]));
  const session = at('osTerminal', 'osFiles', 'ota');
  assert.deepEqual(on(states[session]), ['osFiles', 'osServices', 'osTerminal', 'ota', 'video']);
  // The update ends both, and the state after it has neither.
  assert.deepEqual(['osTerminal', 'osFiles'].filter(name => states[session + 1][name]), []);
});

test('every variable has its switch in the tool\'s source', () => {
  // createSwitches() needs a Mini and a host; its source names each switch as a key of an object or as switches.<name>.
  const source = fs.readFileSync(path.join(here, 'switches.mjs'), 'utf8') + fs.readFileSync(path.join(here, 'os_services.mjs'), 'utf8');
  const missing = VARIABLES.filter(name => !new RegExp(`switches\\.${name} = |^\\s+${name}: `, 'm').test(source));
  assert.deepEqual(missing, []);
});

test('a line that starts with "//" is a comment; a "//" anywhere else is not', () => {
  const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-')), 'table.jsonc');
  fs.writeFileSync(file, '// top\n[\n  // a rule\n  { "issue": "https://example.test/1" }\n]\n');
  assert.deepEqual(readJsonc(file), [{ issue: 'https://example.test/1' }]);
  fs.writeFileSync(file, '[\n  { "max": 5 } // at the end of a line\n]\n');
  assert.throws(() => readJsonc(file));
});

test('the update server announces a higher version for the image it is given', async () => {
  const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-')), 'jetkvm-mini.bin');
  const image = Buffer.alloc(4096, 7);
  image.fill(0, 48, 80);
  image.write('0.5.11-dev.1', 48);
  fs.writeFileSync(file, image);
  const firmware = readFirmware(file);
  assert.equal(firmware.version, '0.5.11-dev.1');
  assert.equal(firmware.elfSha256, '07'.repeat(32));

  const server = await serve('127.0.0.1', firmware);
  try {
    const offer = await (await fetch(`${server.base}/releases?sku=x`)).json();
    assert.equal(offer.systemVersion, '99.0.0');
    assert.equal(offer.systemHash, firmware.sha256);
    const served = Buffer.from(await (await fetch(offer.systemUrl)).arrayBuffer());
    assert.deepEqual(served, image);
  } finally {
    server.close();
  }
});

// GnuPG is the judge of the signature: it verifies what the tool's server
// signs with the key that the server hands out, and that key is the one whose
// fingerprint a bench build has (jetkvm_ota_test_key.h).
test('the update server signs its image with the test release key, which it serves as a keyserver does', async t => {
  if (spawnSync('gpg', ['--version']).status !== 0) return t.skip('needs the gpg command');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const file = path.join(directory, 'jetkvm-mini.bin');
  fs.writeFileSync(file, Buffer.alloc(70_000, 9));
  const header = fs.readFileSync(path.join(here, '..', '..', '..', 'components', 'jetkvm_ota', 'jetkvm_ota_test_key.h'), 'utf8');
  const trusted = /JETKVM_OTA_TEST_RELEASE_KEY_FINGERPRINT "([0-9A-F]{40})"/.exec(header)[1];
  const gpg = (...args) => spawnSync('gpg', ['--batch', '--homedir', directory, '--status-fd', '1', ...args], { encoding: 'utf8' }).stdout;
  const server = await serve('127.0.0.1', readFirmware(file));
  try {
    const offer = await (await fetch(`${server.base}/releases?sku=x`)).json();
    assert.equal(offer.systemSigUrl, `${offer.systemUrl}.sig`);
    fs.writeFileSync(`${file}.sig`, Buffer.from(await (await fetch(offer.systemSigUrl)).arrayBuffer()));
    const key = await fetch(`${server.base}/keyserver/${trusted}`);
    assert.equal(key.status, 200);
    fs.writeFileSync(path.join(directory, 'key.asc'), Buffer.from(await key.arrayBuffer()));
    assert.match(gpg('--import', path.join(directory, 'key.asc')), new RegExp(`IMPORT_OK 1 ${trusted}`));
    // VALIDSIG ends with the fingerprint of the primary key.
    assert.match(gpg('--verify', `${file}.sig`, file), new RegExp(`VALIDSIG .* ${trusted}\\n`));
    fs.appendFileSync(file, 'x');
    assert.match(gpg('--verify', `${file}.sig`, file), /BADSIG/);
    assert.equal((await fetch(`${server.base}/keyserver/${'0'.repeat(40)}`)).status, 404);
  } finally {
    server.close();
    spawnSync('gpgconf', ['--homedir', directory, '--kill', 'all']);
  }
});

test('a server without a firmware image has no key for a keyserver request', async () => {
  const server = await serve('127.0.0.1', null);
  try {
    assert.equal((await fetch(`${server.base}/keyserver/undefined`)).status, 404);
  } finally {
    server.close();
  }
});

test('the update server offers what an integration test asks for: a version, and a signature that is missing, empty, of another key or of another image', async t => {
  if (spawnSync('gpg', ['--version']).status !== 0) return t.skip('needs the gpg command');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const file = path.join(directory, 'jetkvm-mini.bin');
  fs.writeFileSync(file, Buffer.alloc(70_000, 9));
  const firmware = readFirmware(file);
  const offered = async (offer, query = '') => {
    const server = await serve('127.0.0.1', firmware, offer);
    try {
      const answer = await (await fetch(`${server.base}/releases?sku=x${query}`)).json();
      const signature = answer.systemSigUrl && Buffer.from(await (await fetch(answer.systemSigUrl)).arrayBuffer());
      return { answer, signature };
    } finally {
      server.close();
    }
  };
  const none = await offered({ version: '99.0.0-dev.1', signature: 'none' });
  assert.equal(none.answer.systemVersion, '99.0.0-dev.1');
  assert.equal('systemSigUrl' in none.answer, false);
  assert.equal((await offered({ signature: 'empty' })).signature.length, 0);
  assert.equal((await offered({}, '&systemVersion=1.2.3')).answer.systemVersion, '1.2.3');
  // GnuPG with the test release key: it has no key for the other key's signature.
  const gpg = (...args) => spawnSync('gpg', ['--batch', '--homedir', directory, '--status-fd', '1', ...args], { encoding: 'utf8' }).stdout;
  gpg('--import', path.join(here, '..', '..', '..', 'components', 'jetkvm_ota', 'test_key', 'test-release-key.asc'));
  fs.writeFileSync(`${file}.sig`, (await offered({ signature: 'other key' })).signature);
  const status = gpg('--verify', `${file}.sig`, file);
  // The release key's signature of another image is a bad signature of this one.
  fs.writeFileSync(`${file}.sig`, (await offered({ signature: 'other image' })).signature);
  const otherImage = gpg('--verify', `${file}.sig`, file);
  spawnSync('gpgconf', ['--homedir', directory, '--kill', 'all']);
  assert.match(status, /NO_PUBKEY/);
  assert.doesNotMatch(status, /VALIDSIG|GOODSIG/);
  assert.match(otherImage, /BADSIG/);
});

// The e2e test CA, which a bench build trusts.
const TEST_CA = fs.readFileSync(path.join(here, '..', '..', '..', 'frontend', 'jetkvm-ui', 'e2e', 'mini', 'mock-cloud-ca.pem'));

test('over TLS the update server names the image at its https address, and it ends the mounted image\'s connection once during each download of the firmware', async () => {
  // A firmware image that does not fit into the socket's buffers, so that a client that does not read keeps its download open.
  const firmware = { bytes: Buffer.alloc(64 * 1024 * 1024, 7), sha256: 'a'.repeat(64) };
  const server = await serve('127.0.0.1', firmware);
  // One answer, to a client that trusts the test CA and keeps its connections, as the Mini keeps the image's.
  const agent = new https.Agent({ keepAlive: true });
  const answer = (url, headers = {}) => new Promise((resolve, reject) => {
    const request = https.get(url, { headers, ca: TEST_CA, agent }, response => resolve({ request, response }));
    request.on('error', reject);
  });
  const read = async url => {
    const { response } = await answer(url, { Range: 'bytes=0-511' });
    response.resume();
    return response.headers.connection;
  };
  try {
    const { base, url } = await server.secure();
    const offer = JSON.parse((await ask(`${base}/releases?sku=x`)).body);
    assert.equal(offer.systemUrl, `${base}/jetkvm-mini.bin`);
    // The plain server names its own address, as before.
    assert.equal((await (await fetch(`${server.base}/releases`)).json()).systemUrl, `${server.base}/jetkvm-mini.bin`);
    // Without a download a read's connection stays.
    assert.notEqual(await read(url), 'close');
    // The download begins and stays open: the first read is the connection's last, and the next one's connection stays.
    const download = await answer(offer.systemUrl);
    assert.equal(await read(url), 'close');
    assert.notEqual(await read(url), 'close');
    // A download that ended without a read in it leaves nothing for the next read.
    download.request.destroy();
    const second = await answer(offer.systemUrl);
    second.request.destroy();
    await new Promise(resolve => setTimeout(resolve, 300));
    assert.notEqual(await read(url), 'close');
  } finally {
    agent.destroy();
    server.close();
  }
});

// One request by a client that trusts `ca` and, for http, nothing: { status, body }.
const ask = (url, { method = 'GET', headers = {}, body = null, ca = TEST_CA } = {}) => new Promise((resolve, reject) => {
  const request = (url.startsWith('https:') ? https : http).request(url, { method, headers, ca, agent: false }, response => {
    let text = '';
    response.on('data', chunk => { text += chunk; });
    response.on('end', () => resolve({ status: response.statusCode, body: text }));
  });
  request.on('error', reject);
  request.end(body);
});

test('the cloud\'s certificate names its address, and the test CA vouches for it with the signatures of api.jetkvm.com\'s chain', () => {
  const { key, cert } = serverCertificate('192.0.2.7');
  const [server, intermediate, ...more] = cert.match(/-----BEGIN CERTIFICATE-----[^-]+-----END CERTIFICATE-----/g)
    .map(pem => new X509Certificate(pem));
  assert.deepEqual(more, []);
  assert.ok(server.verify(intermediate.publicKey));
  assert.ok(intermediate.verify(new X509Certificate(TEST_CA).publicKey));
  assert.ok(server.checkPrivateKey(createPrivateKey(key)));
  assert.equal(server.ca, false);
  assert.equal(server.checkIP('192.0.2.7'), '192.0.2.7');
  assert.equal(server.checkIP('192.0.2.8'), undefined);
  // A P-256 key for the handshake and one P-256 and one P-384 signature on
  // the chain: the intermediate's key signs the server's certificate.
  assert.equal(server.publicKey.asymmetricKeyDetails.namedCurve, 'prime256v1');
  assert.equal(intermediate.publicKey.asymmetricKeyDetails.namedCurve, 'secp384r1');
  assert.equal(new X509Certificate(TEST_CA).publicKey.asymmetricKeyDetails.namedCurve, 'prime256v1');

  const named = new X509Certificate(serverCertificate('cloud.test').cert);
  assert.equal(named.checkHost('cloud.test'), 'cloud.test');
  assert.equal(named.checkIP('192.0.2.7'), undefined);
  // A URL has an IPv6 address in brackets; the certificate names the address.
  assert.equal(new X509Certificate(serverCertificate(new URL('http://[2001:db8::7]:8791/').hostname).cert).checkIP('2001:db8::7'), '2001:db8::7');
});

// A port for a cloud of a test: one that the kernel picks, so that nothing
// listens on it and no connection of this machine has it as its own port. A
// fixed port in the range for outgoing connections fails with EADDRINUSE
// for as long as another program's connection has it.
const freePort = () => new Promise((resolve, reject) => {
  const probe = net.createServer();
  probe.once('error', reject);
  probe.listen(0, () => {
    const { port } = probe.address();
    probe.close(() => resolve(port));
  });
});

test('only a client that trusts the test CA and asks for the cloud\'s address gets through its TLS', async () => {
  const port = await freePort();
  const cloud = startFakeCloud(`http://127.0.0.1:${port}/oidc/jwks`);
  try {
    assert.equal(cloud.url, `https://127.0.0.1:${port}`);
    assert.deepEqual(cloud.handshakes(), { handshakes: 0, handshakeMs: null });
    assert.equal((await ask(`${cloud.url}/oidc/jwks`)).status, 200);
    assert.equal(cloud.handshakes().handshakes, 1);
    assert.ok(cloud.handshakes().handshakeMs >= 0);

    assert.equal(cloud.problem(), null);

    // The same client with the public authorities only, and with the test CA for another name of this machine.
    await assert.rejects(ask(`${cloud.url}/oidc/jwks`, { ca: tls.rootCertificates }),
      error => /^(UNABLE_TO_GET_ISSUER_CERT|UNABLE_TO_GET_ISSUER_CERT_LOCALLY|SELF_SIGNED_CERT_IN_CHAIN|UNABLE_TO_VERIFY_LEAF_SIGNATURE)$/.test(error.code));
    await assert.rejects(ask(`https://localhost:${port}/oidc/jwks`), { code: 'ERR_TLS_CERT_ALTNAME_INVALID' });

    // A client that ends the handshake with an alert about the certificate, as the Mini's TLS does: the cloud says why.
    cloud.adoption();
    const client = spawn('openssl', ['s_client', '-connect', `127.0.0.1:${port}`, '-verify_return_error'],
      { stdio: ['pipe', 'ignore', 'ignore'] });
    client.stdin.end();
    assert.notEqual(await new Promise(resolve => client.on('close', resolve)), 0);
    await until(() => cloud.problem(), 2000, 'the cloud did not see the handshake end');
    assert.match(cloud.problem(), /^a TLS handshake with the Mini failed: .*alert unknown ca/);
  } finally {
    cloud.close();
  }
});

test('the https cloud refuses a request without TLS; the http cloud takes both', async () => {
  const securePort = await freePort();
  const secure = startFakeCloud(`http://127.0.0.1:${securePort}/oidc/jwks`);
  // The first cloud listens before the second port is picked, so the two differ.
  const plainPort = await freePort();
  const plain = startFakeCloud(`http://127.0.0.1:${plainPort}/oidc/jwks`, { tls: false });
  try {
    assert.equal((await ask(`http://127.0.0.1:${securePort}/oidc/jwks`)).status, 400);
    assert.match(secure.problem(), /GET \/oidc\/jwks without TLS: its build fetches the keys over http/);
    secure.adoption();
    assert.equal(secure.problem(), null);

    assert.equal(plain.url, `http://127.0.0.1:${plainPort}`);
    assert.equal((await ask(`http://127.0.0.1:${plainPort}/oidc/jwks`)).status, 200);
    assert.equal((await ask(`https://127.0.0.1:${plainPort}/oidc/jwks`)).status, 200);
    assert.equal(plain.problem(), null);
  } finally {
    secure.close();
    plain.close();
  }
});

for (const secure of [true, false]) {
  test(`the fake cloud signs a login, exchanges the token once, and holds the socket of that token, over ${secure ? 'https and wss' : 'http and ws'}`, async () => {
    const port = await freePort();
    const cloud = startFakeCloud(`http://127.0.0.1:${port}/oidc/jwks`, { tls: secure });
    try {
      const adoption = cloud.adoption();
      const { keys } = JSON.parse((await ask(`${cloud.url}/oidc/jwks`)).body);
      const [head, claims, signature] = adoption.oidcGoogle.split('.');
      assert.equal(JSON.parse(Buffer.from(head, 'base64url')).kid, keys[0].kid);
      assert.ok(verify('sha256', Buffer.from(`${head}.${claims}`), createPublicKey({ key: keys[0], format: 'jwk' }),
        Buffer.from(signature, 'base64url')));

      const exchange = () => ask(`${cloud.url}/devices/token`, { method: 'POST', body: JSON.stringify({ tempToken: adoption.token }) });
      const { secretToken } = JSON.parse((await exchange()).body);
      assert.equal((await exchange()).status, 404);

      // A WebSocket with the token: the handshake, then a masked ping and its pong.
      const open = authorization => new Promise(resolve => {
        const socket = secure ? tls.connect({ host: '127.0.0.1', port, ca: TEST_CA }) : net.connect(port, '127.0.0.1');
        socket.once(secure ? 'secureConnect' : 'connect', () => socket.write('GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n' +
          `Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nX-App-Version: 1.2.3-mini\r\nAuthorization: ${authorization}\r\n\r\n`));
        socket.once('data', data => resolve({ socket, answer: data.toString() }));
        socket.once('close', () => resolve({ socket, answer: '' }));
        socket.on('error', () => {});
      });
      assert.equal((await open('Bearer wrong')).answer, '');
      assert.equal(cloud.connected(), false);
      // One fetch of the key set, one exchange (the second was refused), and no socket yet.
      assert.deepEqual(cloud.asked(), { keyFetches: 1, tokenExchanges: 1, sockets: 0, appVersion: null });
      // A socket that the cloud drops is closed for the client, and the next one is taken.
      const dropped = await open(`Bearer ${secretToken}`);
      const closed = new Promise(resolve => dropped.socket.once('close', resolve));
      cloud.drop();
      await closed;
      assert.equal(cloud.connected(), false);
      const { socket, answer } = await open(`Bearer ${secretToken}`);
      assert.deepEqual(cloud.asked(), { keyFetches: 1, tokenExchanges: 1, sockets: 2, appVersion: '1.2.3-mini' });
      assert.match(answer, /^HTTP\/1\.1 101 /);
      assert.match(answer, /Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK\+xOo=/);
      assert.equal(cloud.connected(), true);
      const pong = new Promise(resolve => socket.once('data', resolve));
      socket.write(Buffer.from([0x89, 0x82, 1, 2, 3, 4, 0x68 ^ 1, 0x69 ^ 2]));
      assert.deepEqual([...await pong], [0x8a, 2, 0x68, 0x69]);

      assert.equal((await ask(`${cloud.url}/devices/x`, { method: 'DELETE', headers: { Authorization: `Bearer ${secretToken}` } })).status, 204);
      // Each request and each socket was one connection, with a TLS handshake on the https cloud.
      assert.equal(cloud.handshakes().handshakes, secure ? 7 : 0);
      socket.destroy();
    } finally {
      cloud.close();
    }
  });
}

// A Mini for an adoption: its /adopt page, its cloud state, and a
// registration that exchanges the token at the cloud, as the Mini does.
// refuse: the registration answers 500 without asking the cloud.
// cloudUiVersion: what the registration's answer names, or undefined.
const startMini = ({ cloudUrl, cloudUiVersion, refuse = false }) => new Promise(resolve => {
  const server = http.createServer((request, response) => {
    const { pathname } = new URL(request.url, 'http://localhost');
    const json = (status, body) => response.writeHead(status, { 'Content-Type': 'application/json' }).end(JSON.stringify(body));
    if (request.method === 'GET' && pathname === '/adopt') return response.writeHead(200, { 'Content-Type': 'text/html' }).end('<!doctype html>');
    if (request.method === 'GET' && pathname === '/cloud/state') return json(200, { connected: false, url: cloudUrl, appUrl: cloudUrl });
    if (request.method !== 'POST' || pathname !== '/cloud/register') return json(404, {});
    let body = '';
    request.on('data', chunk => { body += chunk; });
    request.on('end', async () => {
      if (refuse) return json(500, { error: 'Failed to exchange token' });
      const exchange = await ask(`${cloudUrl}/devices/token`, { method: 'POST', body: JSON.stringify({ tempToken: JSON.parse(body).token }) });
      if (exchange.status !== 200) return json(exchange.status, { error: 'Failed to exchange token' });
      json(200, { message: 'Cloud registration successful', ...(cloudUiVersion ? { cloudUiVersion } : {}) });
    });
  });
  server.listen(0, '127.0.0.1', () => resolve({ url: `http://127.0.0.1:${server.address().port}`, close: () => server.close() }));
});

// What adopt() uses of a browser's page, for a browser that follows
// redirects and, on a Mini's /adopt page, does what that page's loader does
// (frontend/jetkvm-ui/src/routes/adopt.tsx): it asks the Mini for its cloud
// state, gives it the token and the login of its own address, and goes to
// the name page under the cloud's app address. slowMs: how long a load of
// a page takes before its request.
function browserPage({ slowMs = 0 } = {}) {
  const listeners = { request: new Set(), response: new Set() };
  const emit = (event, value) => { for (const listener of listeners[event]) listener(value); };
  let at = 'about:blank';
  const fetchPage = address => new Promise((resolve, reject) => {
    const request = (address.startsWith('https:') ? https : http).request(address, { ca: TEST_CA, agent: false }, response => {
      response.resume();
      response.on('end', () => resolve(response));
    });
    request.on('error', reject);
    request.end();
  });
  const adoptPage = async address => {
    const query = new URL(address).searchParams;
    const state = await ask(new URL('/cloud/state', address).toString());
    const registerUrl = new URL('/cloud/register', address).toString();
    emit('request', { isNavigationRequest: () => false, url: () => registerUrl });
    const registration = await ask(registerUrl, { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ token: query.get('tempToken'), oidcGoogle: query.get('oidcGoogle'), clientId: query.get('clientId') }) });
    const ok = registration.status >= 200 && registration.status < 300;
    emit('response', { url: () => registerUrl, ok: () => ok, status: () => registration.status, text: async () => registration.body });
    if (!ok) return;
    const version = JSON.parse(registration.body).cloudUiVersion;
    await page.goto(`${JSON.parse(state.body).appUrl}${version ? `/v/${version}` : ''}/devices/${query.get('deviceId')}/setup`);
  };
  const page = {
    on: (event, listener) => listeners[event].add(listener),
    off: (event, listener) => listeners[event].delete(listener),
    url: () => at,
    goto: async address => {
      if (slowMs) await new Promise(resolve => setTimeout(resolve, slowMs));
      for (let next = address; next;) {
        emit('request', { isNavigationRequest: () => true, url: () => next });
        const response = await fetchPage(next);
        at = next;
        next = response.statusCode === 302 ? response.headers.location : null;
      }
      if (new URL(at).pathname === '/adopt') adoptPage(at).catch(() => undefined);
    },
    waitForURL: async (matches, { timeout }) => {
      await until(() => matches(new URL(at)), timeout, `Timeout ${timeout}ms exceeded`);
    },
  };
  return page;
}

for (const cloudUiVersion of ['0.5.11-mini', undefined]) {
  test(`an adoption goes from the cloud's sign-in page through the Mini's /adopt page to the name page ${cloudUiVersion ? 'of the cloud UI\'s version' : 'without a version'}, and has the times of that way`, async () => {
    const cloud = startFakeCloud(`http://127.0.0.1:${await freePort()}/oidc/jwks`);
    const mini = await startMini({ cloudUrl: cloud.url, cloudUiVersion });
    try {
      // The address that the "Adopt" button of the settings page makes, from a device page with a path and a query.
      const signIn = cloud.signInPage('abc123', `${mini.url}/devices/local/settings/access?x=1#y`);
      assert.equal(signIn, `${cloud.url}/signup?deviceId=abc123&returnTo=${mini.url}/adopt`);
      const page = browserPage();
      const before = Date.now();
      const times = await adopt(page, signIn, cloud.isNamePage, 5000);
      assert.equal(page.url(), `${cloud.url}${cloudUiVersion ? `/v/${cloudUiVersion}` : ''}/devices/abc123/setup`);
      assert.deepEqual(Object.keys(times), ['adoptAt', 'registerAt', 'registeredAt', 'namePageAt']);
      assert.deepEqual(Object.values(times), Object.values(times).sort((a, b) => a - b));
      assert.ok(before <= times.adoptAt && times.namePageAt <= Date.now());
      // The Mini's exchange of the token was its one TLS handshake with the
      // cloud; the browser's handshakes for the two pages are not counted.
      assert.equal(cloud.handshakes().handshakes, 1);
    } finally {
      mini.close();
      cloud.close();
    }
  });
}

test('an adoption that the Mini refuses fails at once with the Mini\'s answer, and one that does not reach the name page fails with where the browser is', async () => {
  const cloud = startFakeCloud(`http://127.0.0.1:${await freePort()}/oidc/jwks`);
  const refusing = await startMini({ cloudUrl: cloud.url, refuse: true });
  // A Mini whose cloud's app is at another address than the cloud's name page.
  const elsewhere = await startMini({ cloudUrl: cloud.url });
  try {
    const started = Date.now();
    await assert.rejects(adopt(browserPage(), cloud.signInPage('abc123', refusing.url), cloud.isNamePage, 5000),
      { message: 'the Mini did not take the adoption: HTTP 500 {"error":"Failed to exchange token"}' });
    assert.ok(Date.now() - started < 2000);
    const notThere = `the browser did not get from the cloud's sign-in page to its name page; it is at ${cloud.url}/devices/abc123/setup: Timeout `;
    await assert.rejects(adopt(browserPage(), cloud.signInPage('abc123', elsewhere.url), () => false, 600),
      error => error.message.startsWith(notThere));
    // The limit is for the whole way: a browser that needed 700 ms of 1000 to
    // get to the /adopt page has 300 ms left for the name page, not 1000.
    const slowStarted = Date.now();
    await assert.rejects(adopt(browserPage({ slowMs: 700 }), cloud.signInPage('abc123', elsewhere.url), () => false, 1000),
      error => /^the browser did not get from the cloud's sign-in page to its name page; it is at .*: Timeout /.test(error.message));
    assert.ok(Date.now() - slowStarted < 1500, `the adoption gave up after ${Date.now() - slowStarted} ms`);
    // The sign-in page needs an address to return to.
    assert.equal((await ask(`${cloud.url}/signup?deviceId=abc123`)).status, 400);
  } finally {
    refusing.close();
    elsewhere.close();
    cloud.close();
  }
});

// Switches that only record their calls. worksAfterMs: how long after the
// last call of on() or off() each variable works again.
function fakeSwitches(calls, { failing = null, worksAfterMs = 0 } = {}) {
  const on = {};
  let changedAt = 0;
  return Object.fromEntries(['video', 'mount', 'ota'].map(name => [name, {
    on: async () => {
      if (name === failing) throw new Error('refused');
      calls.push(`${name} on`);
      on[name] = true;
      changedAt = Date.now();
    },
    off: async () => { calls.push(`${name} off`); on[name] = false; changedAt = Date.now(); },
    isOn: async () => Boolean(on[name]),
    works: () => Date.now() - changedAt >= worksAfterMs,
  }]));
}

test('--without leaves out the states of the named variables and keeps the rest in order', () => {
  const walk = readJsonc(path.join(here, '..', 'lists.jsonc')).walk;
  const short = withoutVariables(walk, ['ota', 'upload']);
  assert.equal(short.some(step => step.state.ota || step.state.upload), false);
  assert.deepEqual(short, walk.filter(step => !step.state.ota && !step.state.upload));
  assert.equal(short.length, walk.length - 6);
  assert.deepEqual(withoutVariables(walk, []), walk);
});

test('the driver turns variables on in their order and off in the reverse order', async () => {
  const calls = [];
  const rows = [];
  const list = [
    { state: { ota: true, mount: true, video: true }, holdSeconds: 0 },
    { state: {}, holdSeconds: 0 },
  ];
  assert.equal(await drive(list, fakeSwitches(calls), {}, row => rows.push(row)), true);
  assert.deepEqual(calls, ['video on', 'mount on', 'ota on', 'ota off', 'mount off', 'video off']);
  assert.deepEqual(rows.map(row => row.change), [
    { video: true }, { mount: true }, { ota: true }, { ota: false }, { mount: false }, { video: false },
  ]);
  assert.deepEqual(rows[1].state, { video: true, mount: true });
});

test('a change is finished only when its variable works', async () => {
  const rows = [];
  const list = [{ state: { video: true }, holdSeconds: 0 }, { state: { video: true, mount: true }, holdSeconds: 0 }];
  await drive(list, fakeSwitches([], { worksAfterMs: 600 }), {}, row => rows.push(row));
  for (const row of rows) assert.ok(row.seconds >= 0.6, `${JSON.stringify(row.change)} took ${row.seconds} s`);
});

test('the driver records a refused change and ends there', async () => {
  const calls = [];
  const rows = [];
  const state = {};
  const list = [
    { state: { video: true }, holdSeconds: 0 },
    { state: { video: true, mount: true, ota: true }, holdSeconds: 0 },
  ];
  assert.equal(await drive(list, fakeSwitches(calls, { failing: 'mount' }), state, row => rows.push(row)), false);
  assert.deepEqual(calls, ['video on']);
  assert.equal(rows.at(-1).error, 'refused');
  assert.deepEqual(rows.at(-1).change, { mount: true });
  assert.deepEqual(state, { video: true, mount: true });
});

// Switches for the tests of the driver's wait. Each has its value at once,
// except `stuck`, which never gets it. works[name](ms, on) is what works()
// returns that long after the variable's last change; without it, true.
function timedSwitches(calls, { works = {}, stuck = null } = {}) {
  const on = {};
  const changedAt = {};
  return Object.fromEntries(['video', 'osServices', 'mount', 'ota'].map(name => [name, {
    on: async () => { calls.push(`${name} on`); on[name] = true; changedAt[name] = Date.now(); },
    off: async () => { calls.push(`${name} off`); on[name] = false; changedAt[name] = Date.now(); },
    isOn: async () => name !== stuck && Boolean(on[name]),
    works: () => (works[name] ? works[name](Date.now() - changedAt[name], on[name]) : true),
  }]));
}
// Runs the driver on a clock that the test moves from one timer of the
// driver to the next, and returns its result and its rows as the recorder
// writes them, with the time of each.
//
// With `fps`, a recorder runs on the same clock and the driver waits for
// each state to settle. It writes one sample per second, 0.7 s after the
// full second, from 2.3 s before the drive: the driver's state at that
// time, a Mini that answers, the frames that fps(at, state) gives for the
// sample at `at` seconds after the start, and the loads of loads(at, state).
// `state` is the state in which the drive begins. The result then also has
// `recording`: the samples and the change rows.
async function driveOnFakeClock(list, switches, criteria = {}, known = [], { state = {}, fps = null, loads = () => ({}) } = {}) {
  const real = { now: Date.now, setTimeout: globalThis.setTimeout };
  let clock = T0;
  const timers = [];
  Date.now = () => clock;
  globalThis.setTimeout = (callback, ms) => { timers.push({ at: clock + ms, callback }); };
  try {
    const rows = [];
    const samples = [];
    let frames = 0;
    const sample = t => {
      const at = (t - T0) / 1000;
      frames += fps(at, state);
      samples.push({ t, state: { ...state }, device: { nowMs: 60_000 + t - T0 },
        browser: { connectionState: 'connected', video: { framesDecoded: frames } }, loads: loads(at, state) });
    };
    if (fps) {
      for (const before of [-2300, -1300, -300]) sample(T0 + before);
      const tick = () => {
        sample(clock);
        globalThis.setTimeout(tick, 1000);
      };
      globalThis.setTimeout(tick, 700);
    }
    let outcome = null;
    drive(list, switches, state, row => rows.push({ t: Date.now(), ...row }), () => false,
      (change, at) => changeLimit(criteria, known, change, at),
      fps && (pending => secondsNotNormal([...samples, ...rows, pending], criteria, known)))
      .then(completed => { outcome = { completed }; }, error => { outcome = { error }; });
    while (!outcome) {
      // What the driver can do without the clock, then the next timer at its time.
      await new Promise(resolve => setImmediate(resolve));
      timers.sort((a, b) => a.at - b.at);
      const next = timers.shift();
      if (next) {
        clock = next.at;
        next.callback();
      }
    }
    if (outcome.error) throw outcome.error;
    return { completed: outcome.completed, rows, recording: [...samples, ...rows] };
  } finally {
    Date.now = real.now;
    globalThis.setTimeout = real.setTimeout;
  }
}
const MOUNT_IN_7 = { changeTakesAtMost: [{ change: { video: true }, max: 4 }, { change: { mount: true }, max: 7 }] };

test('a change that does not work when its limit and the margin are over is recorded as not finished, the walk goes on, and the judge reports it', async () => {
  const calls = [];
  const list = [
    { state: { video: true }, holdSeconds: 0 },
    { state: { video: true, mount: true, ota: true }, holdSeconds: 2 },
    { state: {}, holdSeconds: 0 },
  ];
  const switches = timedSwitches(calls, { works: { mount: (ms, on) => !on || 'the host read nothing' } });
  const { completed, rows, recording: recorded } = await driveOnFakeClock(list, switches, MOUNT_IN_7, [], { fps: () => 30 });
  assert.equal(completed, true);
  assert.deepEqual(calls, ['video on', 'mount on', 'ota on', 'ota off', 'mount off', 'video off']);
  const mount = rows.find(row => row.change.mount === true);
  // The limit of 7 s and the driver's margin of 5 s.
  assert.deepEqual([mount.seconds, mount.error, mount.unfinished], [12, null, 'mount is on and does not work: the host read nothing']);
  assert.deepEqual(mount.state, { video: true, mount: true });
  assert.deepEqual(rows.filter(row => row.unfinished), [mount]);

  const verdict = judge(recorded, MOUNT_IN_7);
  assert.equal(verdict.result, 'fail');
  assert.deepEqual(verdict.failures.map(f => [f.change.change, f.value, f.max]), [[{ mount: true }, 12, 7]]);
  assert.match(report(verdict, T0), /^FAIL {2}mount on was not finished after 12\.0 s, limit 7 s: mount is on and does not work: the host read nothing$/m);
});

test('on both boards no picture rule judges a display mode change during an upload, and the end of the upload asks for the picture of the new mode', () => {
  // Seconds 0 to 5: the upload and the other mode, without a frame. The
  // upload ends from 5.5 s to 7 s; the picture is back in second 7, and the
  // browser counts the time without it as one freeze then.
  const rows = ({ heightAfter = 720, backAt = 7 } = {}) => {
    let frames = 0;
    const samples = range(0, 11).map(second => {
      if (second >= backAt) frames += 30;
      return { t: T0 + second * 1000, state: { video: true, upload: second < 6, hostModeChange: true }, device: { nowMs: 60_000 + second * 1000 },
        browser: { connectionState: 'connected', video: { framesDecoded: frames, frameHeight: second >= backAt ? heightAfter : 1080,
          totalFreezesDuration: second >= backAt ? 7 : 0 } }, loads: {} };
    });
    return [...samples, { t: T0 + 7000, change: { upload: false }, state: { video: true, upload: false, hostModeChange: true },
      startedAt: T0 + 5500, seconds: 1.5, switchSeconds: 1.5, error: null }];
  };
  const picture = ['framesPerSecond', 'freezeMs', 'videoHeight'];
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const failed = recorded => judge(recorded, criteria).failures.filter(f => picture.includes(f.measure)).map(f => [f.measure, f.t - T0, f.value]);
    assert.deepEqual(failed(rows()), [], board);
    // The picture of the mode before is the wrong one after the upload.
    assert.deepEqual(failed(rows({ heightAfter: 1080 })), [['videoHeight', 8000, 1080]], board);
    // No picture after the upload fails by the frame rate's own rule.
    assert.ok(failed(rows({ backAt: 99 })).some(f => f[0] === 'framesPerSecond'), board);
    // Without an upload the change itself waits for the picture: the size of
    // the mode before is no line until its switch is done, at 3 s.
    let frames = 0;
    const alone = range(0, 6).map(second => {
      if (second < 1 || second >= 3) frames += 30;
      return { t: T0 + second * 1000, state: { video: true, hostModeChange: second >= 1 }, device: { nowMs: 60_000 + second * 1000 },
        browser: { connectionState: 'connected', video: { framesDecoded: frames, frameHeight: second >= 3 ? 720 : 1080, totalFreezesDuration: second >= 3 ? 2 : 0 } }, loads: {} };
    });
    assert.deepEqual(failed([...alone, { t: T0 + 3000, change: { hostModeChange: true }, state: { video: true, hostModeChange: true },
      startedAt: T0 + 500, seconds: 2.5, switchSeconds: 2.5, error: null }]), [], board);
  }
});

test('on both boards a freeze and a slow encoder during an upload with a display mode change are no line, and the same after the upload are', () => {
  // HDMI video goes on during the upload, as on the Ethernet board. The upload
  // with the other mode runs in seconds 0 to 5 and ends from 5.5 s to 7 s.
  // From second `bad` on, for four seconds: the encoder needs 40 ms a frame,
  // the browser gets no frame in the second after `bad` and counts a freeze
  // of 1 s in the one after that, the quantizer is 45, and the Mini drops a
  // frame that is too big and the browser loses a packet in the last one.
  const rows = bad => {
    let frames = 0, qp = 0, encoded = 0, encodeUs = 0, freezes = 0, oversize = 0, lost = 0;
    const samples = range(0, 12).map(second => {
      const slow = second >= bad && second < bad + 4;
      if (second !== bad + 1) {
        frames += 30;
        qp += 30 * (slow && second >= bad + 2 ? 45 : 25);
      }
      if (second === bad + 2) freezes += 1;
      if (second === bad + 3) {
        oversize += 1;
        lost += 1;
      }
      encoded += 30;
      encodeUs += 30 * (slow ? 40_000 : 20_000);
      return { t: T0 + second * 1000, state: { video: true, upload: second < 6, hostModeChange: true },
        device: { nowMs: 60_000 + second * 1000, accessUnitsOversize: oversize,
          videoEncode: { pFrames: encoded, pEncodeUs: encodeUs, idrFrames: encoded / 15, idrEncodeUs: encodeUs / 15 } },
        browser: { connectionState: 'connected', video: { framesDecoded: frames, qpSum: qp, frameHeight: 720,
          totalFreezesDuration: freezes, packetsLost: lost } }, loads: {} };
    });
    return [...samples,
      { t: T0 + 1000, change: { hostModeChange: true }, state: { video: true, upload: true, hostModeChange: true },
        startedAt: T0 + 500, seconds: 0.2, switchSeconds: 0.2, error: null },
      { t: T0 + 7000, change: { upload: false }, state: { video: true, upload: false, hostModeChange: true },
        startedAt: T0 + 5500, seconds: 1.5, switchSeconds: 1.5, error: null }];
  };
  const picture = ['framesPerSecond', 'freezeMs', 'videoQuantizer', 'videoHeight', 'videoPacketsLost', 'encodeMs', 'keyframeEncodeMs', 'framesTooBig'];
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const failed = recorded => [...new Set(judge(recorded, criteria).failures.filter(f => picture.includes(f.measure)).map(f => f.measure))].sort();
    assert.deepEqual(failed(rows(1)), [], board);
    const after = ['encodeMs', 'freezeMs', 'framesTooBig', 'keyframeEncodeMs', 'videoQuantizer'];
    if (board === 'ethernet') after.push('videoPacketsLost');
    assert.deepEqual(failed(rows(8)), after.sort(), board);
  }
});

test('a variable that does not get its value in time is not finished either, and a change a little over its limit is recorded with its time', async () => {
  const list = [{ state: { video: true, mount: true }, holdSeconds: 0 }];
  const stuck = await driveOnFakeClock(list, timedSwitches([], { stuck: 'mount' }), MOUNT_IN_7);
  assert.deepEqual([stuck.completed, stuck.rows[1].seconds, stuck.rows[1].unfinished], [true, 12, 'mount is still off']);

  // 9 s: over the limit of 7 s, within the margin. The judge has the time.
  const slow = await driveOnFakeClock(list, timedSwitches([], { works: { mount: ms => ms >= 9000 } }), MOUNT_IN_7, [], { fps: () => 30 });
  assert.deepEqual([slow.rows[1].seconds, slow.rows[1].unfinished], [9, undefined]);
  assert.match(report(judge(slow.recording, MOUNT_IN_7), T0), /^FAIL {2}mount on settled after 9\.0 s, limit 7 s$/m);
});

// A mount whose state is what the host's watch printed last, as a mount of
// switches.mjs has it. The watch prints a line whenever the driver asks,
// except in the silentMs after the unmount; with `ends`, its SSH session
// closes at the unmount.
function watchedMount({ silentMs = 0, ends = false } = {}) {
  const watch = { medium: null, at: Date.now(), ended: false };
  let mounted = false;
  let unmountedAt = null;
  return {
    on: async () => { mounted = true; },
    off: async () => { mounted = false; unmountedAt = Date.now(); watch.ended = ends; },
    isOn: async () => {
      if (!watch.ended && (unmountedAt === null || Date.now() - unmountedAt >= silentMs)) {
        watch.medium = mounted ? { type: 'disk' } : null;
        watch.at = Date.now();
      }
      return mediumOnHost(watch, 'disk', 'host');
    },
  };
}
const UNMOUNT_IN_7 = { changeTakesAtMost: [{ change: { mount: false }, max: 7 }] };

test('an unmount waits for a host watch that is behind: the change has the time it took, and the walk goes on', async () => {
  const list = [{ state: { video: true, mount: true }, holdSeconds: 0 }, { state: {}, holdSeconds: 0 }];
  const unmount = result => result.rows.find(row => row.change.mount === false);

  const behind = await driveOnFakeClock(list, { ...timedSwitches([]), mount: watchedMount({ silentMs: 5000 }) }, UNMOUNT_IN_7);
  assert.deepEqual([behind.completed, unmount(behind).seconds, unmount(behind).error, unmount(behind).unfinished], [true, 5, null, undefined]);
  assert.deepEqual(behind.rows.at(-1).change, { video: false });

  // Silent for longer than the unmount's limit and the driver's margin.
  const late = await driveOnFakeClock(list, { ...timedSwitches([]), mount: watchedMount({ silentMs: 20_000 }) }, UNMOUNT_IN_7);
  assert.deepEqual([late.completed, unmount(late).seconds, unmount(late).error, unmount(late).unfinished], [true, 12, null, 'mount is still on']);
});

test('a host watch that has ended, or is silent for 30 s, ends the walk at the change that asked it', async () => {
  const list = [{ state: { video: true, mount: true }, holdSeconds: 0 }, { state: {}, holdSeconds: 0 }];

  const ended = await driveOnFakeClock(list, { ...timedSwitches([]), mount: watchedMount({ ends: true }) });
  assert.equal(ended.completed, false);
  assert.deepEqual([ended.rows.at(-1).change, ended.rows.at(-1).seconds, ended.rows.at(-1).error],
    [{ mount: false }, 0, "host does not report the Mini's drives"]);

  // No rule names the unmount here, so the driver would wait 180 s for it.
  const silent = await driveOnFakeClock(list, { ...timedSwitches([]), mount: watchedMount({ silentMs: 600_000 }) });
  assert.equal(silent.completed, false);
  assert.deepEqual([silent.rows.at(-1).change, silent.rows.at(-1).seconds, silent.rows.at(-1).error],
    [{ mount: false }, 30, "host does not report the Mini's drives"]);
});

test('the driver waits for a change as long as its known failure allows in that state, and for a change without a rule far longer', async () => {
  const known = [{ issue: 'https://example.test/issues/1', change: { mount: true }, while: { osServices: true }, max: 30 }];
  const after20 = () => timedSwitches([], { works: { mount: ms => ms >= 20_000 } });
  // Beside an OS Services session the entry applies: 20 s is waited for and is known.
  const beside = await driveOnFakeClock([{ state: { video: true, osServices: true, mount: true }, holdSeconds: 0 }], after20(), MOUNT_IN_7, known, { fps: () => 30 });
  const mount = beside.rows.find(row => row.change.mount);
  assert.deepEqual([mount.seconds, mount.unfinished], [20, undefined]);
  const verdict = judge(beside.recording, MOUNT_IN_7, known);
  assert.deepEqual([verdict.result, verdict.known.map(k => k.seconds)], ['pass', [20]]);
  // Without the session the entry does not apply: the rule's 7 s and the margin.
  const alone = await driveOnFakeClock([{ state: { video: true, mount: true }, holdSeconds: 0 }], after20(), MOUNT_IN_7, known);
  assert.deepEqual([alone.rows[1].seconds, alone.rows[1].unfinished], [12, 'mount is on and does not work']);
  // No rule and no entry name "ota on": 20 s is waited for.
  const unruled = await driveOnFakeClock([{ state: { ota: true }, holdSeconds: 0 }], timedSwitches([], { works: { ota: ms => ms >= 20_000 } }), MOUNT_IN_7, known);
  assert.deepEqual([unruled.rows[0].seconds, unruled.rows[0].unfinished], [20, undefined]);
  // An entry of another host gives the change no more time, nor does an entry for a measure during the change.
  const others = [
    { issue: 'https://example.test/issues/2', host: 'macos', change: { mount: true }, max: 60 },
    { issue: 'https://example.test/issues/3', change: { mount: true }, measure: 'framesPerSecond', max: 90, tolerateSeconds: 5 },
  ];
  assert.equal(changeLimit(MOUNT_IN_7, others, { mount: true }, { video: true, mount: true }, 'linux'), 7);
  assert.equal(changeLimit(MOUNT_IN_7, others, { mount: true }, { video: true, mount: true }, 'macos'), 60);
});

// The settling of a state, on a recording that a recorder writes beside the
// drive (driveOnFakeClock with `fps`). The rule is 26 frames per second; an
// unmount may take 3 s to settle and a mount 7 s.
const FPS_26 = { while: { video: true }, measure: 'framesPerSecond', min: 26, failAfterSeconds: 2 };
const UNMOUNT_IN_3 = { everySecond: [FPS_26], changeTakesAtMost: [{ change: { mount: false }, max: 3 }, { change: { mount: true }, max: 7 }] };
const SLOW_UNMOUNT = { issue: 'https://example.test/issues/269', change: { mount: false }, max: 6 };
// 10 frames in the samples of the named seconds after the start of the
// drive, which are those at 0.7 s, 1.7 s and so on, and 30 in the others.
const low = (...seconds) => at => (at > 0 && seconds.includes(Math.floor(at)) ? 10 : 30);
// One unmount beside video, at the start of the drive.
async function unmount(fps, { known = [], list = [{ state: { video: true }, holdSeconds: 0 }], criteria = UNMOUNT_IN_3 } = {}) {
  const calls = [];
  const result = await driveOnFakeClock(list, timedSwitches(calls), criteria, known, { state: { video: true, mount: true }, fps });
  const verdict = judge(result.recording, criteria, known);
  // The report's lines for failures and known failures, without its last line.
  const lines = report(verdict, T0).split('\n').slice(0, -1).filter(line => /^(FAIL|KNOWN) {2}\S/.test(line));
  return { ...result, calls, row: result.rows.find(row => row.change.mount === false), verdict, lines };
}

test('a change is done when its state is settled: its time is the time until the normal seconds began, and the driver believes them when they cover two seconds', async () => {
  // Nothing is disturbed: the state is settled when the switch is done. The
  // driver goes on with the sample at 2.7 s, the first that ends 2 s later.
  const atOnce = await unmount(low());
  assert.deepEqual([atOnce.row.switchSeconds, atOnce.row.seconds, atOnce.row.unsettled, atOnce.row.waitedFor], [0, 0, undefined, undefined]);
  assert.equal(atOnce.row.t - T0, 2750);
  assert.ok(atOnce.row.t - atOnce.row.startedAt >= SETTLE_CONFIRM_SECONDS * 1000);
  assert.deepEqual([atOnce.verdict.result, atOnce.lines], ['pass', []]);

  // The frame rate is low in the samples at 0.7 s and 1.7 s and normal from
  // 1.7 s on: the samples at 2.7 s and 3.7 s show it, and the change settled
  // after 1.7 s.
  const soon = await unmount(low(0, 1));
  assert.deepEqual([soon.row.switchSeconds, soon.row.seconds, soon.row.t - T0], [0, 1.7, 3750]);
  assert.deepEqual(soon.row.waitedFor, { framesPerSecond: { seconds: 1, last: 10 } });
  assert.equal(changeLine(soon.row, 3), 'mount off: settled after 1.7 s');
  assert.deepEqual([soon.verdict.result, soon.lines], ['pass', []]);
});

test('a second outside a limit keeps a state from being settled when it is the last of as many in a row as the rule\'s failAfterSeconds, and under a rule without that at once', async () => {
  // The rule has failAfterSeconds 2. One low second, at 0.7 s or at 1.7 s,
  // is normal: the state is settled when the switch is done.
  for (const second of [0, 1]) {
    const single = await unmount(low(second));
    assert.deepEqual([single.row.seconds, single.row.t - T0, single.row.waitedFor, single.verdict.result], [0, 2750, undefined, 'pass'], second);
  }
  // Two single low seconds with a normal one between them as well.
  const apart = await unmount(low(0, 2));
  assert.deepEqual([apart.row.seconds, apart.row.t - T0, apart.row.waitedFor], [0, 2750, undefined]);
  // Two low seconds in a row: the second of them is not normal.
  const pair = await unmount(low(1, 2));
  assert.deepEqual([pair.row.seconds, pair.row.t - T0, pair.row.waitedFor, pair.verdict.result],
    [2.7, 4750, { framesPerSecond: { seconds: 1, last: 10 } }, 'pass']);

  // A rule with failAfterSeconds 1 and one without it: each low second is
  // not normal. One normal second is not the end: the frame rate falls
  // again at 2.7 s, and the state is settled from then on.
  for (const failAfterSeconds of [1, undefined]) {
    const criteria = { ...UNMOUNT_IN_3, everySecond: [{ ...FPS_26, failAfterSeconds }] };
    const one = await unmount(low(0), { criteria });
    assert.deepEqual([one.row.seconds, one.row.t - T0, one.row.waitedFor], [0.7, 2750, { framesPerSecond: { seconds: 1, last: 10 } }], failAfterSeconds);
    const relapse = await unmount(low(0, 2), { criteria });
    assert.deepEqual([relapse.row.seconds, relapse.row.t - T0, relapse.row.waitedFor.framesPerSecond.seconds], [2.7, 4750, 2], failAfterSeconds);
  }
});

test('a settling time over the limit of the change fails, an entry for the change makes it known up to its own limit, and the rule for every second is judged as before', async () => {
  // An unmount that may take 1 s to settle, and an entry for 2 s. Under 26
  // frames for two seconds: the rule for every second allows that, and the
  // state is settled 1.7 s after the unmount.
  const criteria = { ...UNMOUNT_IN_3, changeTakesAtMost: [{ change: { mount: false }, max: 1 }] };
  const slowUnmount = { ...SLOW_UNMOUNT, max: 2 };
  const late = await unmount(low(0, 1), { criteria });
  assert.equal(late.row.seconds, 1.7);
  assert.equal(changeLine(late.row, 1), 'mount off: settled after 1.7 s, limit 1 s');
  assert.deepEqual([late.verdict.result, late.lines], ['fail', ['FAIL  mount off settled after 1.7 s, limit 1 s']]);
  assert.match(report(late.verdict, T0), /^ {6}not normal after the switch, which was done after 0\.0 s: framesPerSecond for 1 s, last 10$/m);

  const known = await unmount(low(0, 1), { criteria, known: [slowUnmount] });
  assert.deepEqual([known.verdict.result, known.lines], ['pass', ['KNOWN  mount off settled after 1.7 s, limit 1 s']]);
  assert.deepEqual(known.verdict.known.map(k => k.entries), [[slowUnmount]]);

  // 2.7 s: over the entry's 2 s.
  const over = await unmount(low(1, 2), { criteria, known: [slowUnmount] });
  assert.deepEqual([over.verdict.result, over.lines], ['fail', ['FAIL  mount off settled after 2.7 s, limit 1 s']]);
  assert.deepEqual(over.verdict.failures.map(f => f.worseThan), [[slowUnmount]]);

  // Three seconds in a row are one more than the rule for every second
  // allows: that rule fails, and the settling time of 2.7 s does not.
  const stopped = await unmount(low(0, 1, 2));
  assert.deepEqual([stopped.row.seconds, stopped.lines], [2.7, ['FAIL  framesPerSecond is 10 for more than 2 s']]);
});

test('a state that does not settle in the driver\'s wait fails with what was not normal last, no entry covers it, and the walk goes on', async () => {
  // Video is under the frame rate for 62 s: from the second such second on
  // none is normal, and the rule for every second fails in the third.
  const list = [{ state: { video: true }, holdSeconds: 0 }, { state: { video: true, mount: true }, holdSeconds: 0 }];
  const never = await unmount(at => (at > 0 && at < 62 ? 10 : 30), { known: [SLOW_UNMOUNT], list });
  assert.deepEqual([never.row.seconds, never.row.unsettled, never.row.error], [SETTLE_CAP_SECONDS, 'framesPerSecond is 10', null]);
  assert.equal(changeLine(never.row, 3), 'mount off: did not settle in 60 s: framesPerSecond is 10');
  assert.deepEqual([never.verdict.result, never.lines],
    ['fail', ['FAIL  framesPerSecond is 10 for more than 2 s', 'FAIL  mount off did not settle in 60 s, limit 3 s: framesPerSecond is 10']]);
  assert.deepEqual(never.verdict.failures.filter(f => f.change).map(f => f.worseThan), [[SLOW_UNMOUNT]]);
  // The next change follows. The low frame rate is from before it, so its
  // state is settled when its switch is done.
  assert.deepEqual([never.completed, never.calls], [true, ['mount off', 'mount on']]);
  assert.deepEqual(never.rows.map(row => [row.change, row.seconds]), [[{ mount: false }, 60], [{ mount: true }, 0]]);
});

test('a known failure of the new state counts as normal, so a state settles beside it, and a value outside the entry\'s own limit does not', async () => {
  // From the mount on, video is at 20 frames per second for as long as the
  // state lasts: an entry for the state names that.
  const dim = { issue: 'https://example.test/issues/272', while: { mount: true }, measure: 'framesPerSecond', min: 15, tolerateSeconds: 120 };
  const list = [{ state: { video: true, mount: true }, holdSeconds: 0 }];
  const mounted = async (fps, known) => {
    const result = await driveOnFakeClock(list, timedSwitches([]), UNMOUNT_IN_3, known, { state: { video: true }, fps });
    return { row: result.rows[0], verdict: judge(result.recording, UNMOUNT_IN_3, known) };
  };
  const beside = await mounted(at => (at > 0 ? 20 : 30), [dim]);
  assert.deepEqual([beside.row.seconds, beside.row.unsettled, beside.row.t - T0], [0, undefined, 2750]);
  assert.deepEqual([beside.verdict.result, beside.verdict.known.map(k => [k.measure, k.value])], ['pass', [['framesPerSecond', 20]]]);
  // 10 frames in the first two samples are under the entry's 15: the state
  // is settled from 1.7 s on, at 20 frames per second.
  const dip = await mounted(at => (at > 2 ? 20 : at > 0 ? 10 : 30), [dim]);
  assert.deepEqual([dip.row.seconds, dip.row.waitedFor], [1.7, { framesPerSecond: { seconds: 1, last: 10 } }]);
  // Without the entry the state does not settle.
  const unnamed = await mounted(at => (at > 0 ? 20 : 30), []);
  assert.deepEqual([unnamed.row.seconds, unnamed.row.unsettled], [SETTLE_CAP_SECONDS, 'framesPerSecond is 20']);
});

test('a fault that an entry with "fails" names does not hold up the settling of a change, and outside the entry\'s bounds it does', async () => {
  // A burst at the mount: 20 frames per second for 5 s, which the entry
  // names for up to 6 s and down to 15 frames.
  const burst = { issue: 'https://example.test/issues/317', fails: true, while: { mount: true }, measure: 'framesPerSecond', min: 15, tolerateSeconds: 6 };
  const list = [{ state: { video: true, mount: true }, holdSeconds: 8 }];
  const mounted = async fps => {
    const result = await driveOnFakeClock(list, timedSwitches([]), UNMOUNT_IN_3, [burst], { state: { video: true }, fps });
    const verdict = judge(result.recording, UNMOUNT_IN_3, [burst]);
    return { row: result.rows[0], verdict, lines: report(verdict, T0).split('\n').filter(line => /^FAIL {2}\S/.test(line)) };
  };
  // The state is settled when the switch is done, and the fault is the
  // run's one line.
  const named = await mounted(at => (at > 0 && at < 5 ? 20 : 30));
  assert.deepEqual([named.row.seconds, named.row.waitedFor], [0, undefined]);
  assert.deepEqual(named.lines, ['FAIL  framesPerSecond is 20 for more than 2 s    known fault: #317', 'FAIL  known faults only: #317']);
  // 10 frames per second are under the entry's 15: the state is settled
  // when the burst is over, at 4.7 s, over the mount's 7 s it is not.
  const deeper = await mounted(at => (at > 0 && at < 5 ? 10 : 30));
  assert.deepEqual([deeper.row.seconds, deeper.row.waitedFor], [4.7, { framesPerSecond: { seconds: 4, last: 10 } }]);
  // A burst of 12 s that began with the mount is longer than the entry's
  // 6 s: a session that starts 5 s into it waits from the burst's seventh
  // second on, and is settled when the burst is over, 6.7 s after its start.
  const steps = [{ state: { video: true, mount: true }, holdSeconds: 5 }, { state: { video: true, mount: true, osServices: true }, holdSeconds: 0 }];
  const longer = await driveOnFakeClock(steps, timedSwitches([]), UNMOUNT_IN_3, [burst], { state: { video: true, mount: true }, fps: at => (at > 0 && at < 12 ? 20 : 30) });
  const session = longer.rows.find(row => row.change.osServices);
  assert.deepEqual([session.startedAt - T0, session.seconds, session.waitedFor], [5000, 6.7, { framesPerSecond: { seconds: 6, last: 20 } }]);
});

test('a switch that is on for a moment has its own condition first: what it pauses is normal until the switch is done, and the state settles after that', async () => {
  // A loss that the session is back from after 1.2 s, as packetLoss.
  const LOSS = { everySecond: [FPS_26], changeTakesAtMost: [
    { change: { loss: true }, max: 3, pauses: ['framesPerSecond'] }, { change: { loss: false }, max: 2 }] };
  const list = [{ state: { video: true, loss: true }, holdSeconds: 0 }, { state: { video: true }, holdSeconds: 0 }];
  const lost = async fps => {
    let madeAt = null;
    const loss = {
      on: async () => { madeAt = Date.now(); },
      off: async () => { madeAt = null; },
      isOn: async () => madeAt !== null,
      works: on => !on || Date.now() - madeAt >= 1200,
    };
    const result = await driveOnFakeClock(list, { ...timedSwitches([]), loss }, LOSS, [], { state: { video: true }, fps });
    const lines = report(judge(result.recording, LOSS), T0).split('\n').slice(0, -1).filter(line => /^FAIL {2}\S/.test(line));
    return { on: result.rows.find(row => row.change.loss === true), off: result.rows.find(row => row.change.loss === false), lines };
  };
  // The frame rate is low in the samples at 0.7 s and 1.7 s, whose seconds
  // have a part of the switch: the state is settled when the switch is done.
  const back = await lost(low(0, 1));
  assert.deepEqual([back.on.switchSeconds, back.on.seconds, back.on.waitedFor, back.lines], [1.25, 1.25, undefined, []]);
  // The switch back has no condition of its own and settles at once.
  assert.deepEqual([back.off.switchSeconds, back.off.seconds], [0, 0]);
  // Low in the sample at 2.7 s as well, after the switch: that second is
  // the state's first low one, which the rule allows.
  const slower = await lost(low(0, 1, 2));
  assert.deepEqual([slower.on.switchSeconds, slower.on.seconds, slower.on.waitedFor, slower.lines], [1.25, 1.25, undefined, []]);
  // And at 3.7 s: the state's second low one in a row, and the change
  // settled after 3.7 s, over its limit.
  const slow = await lost(low(0, 1, 2, 3));
  assert.deepEqual([slow.on.seconds, slow.on.waitedFor, slow.lines],
    [3.7, { framesPerSecond: { seconds: 1, last: 10 } }, ['FAIL  loss on settled after 3.7 s, limit 3 s']]);
});

test('a switch that ends other variables has them turned off when it is done, and its state settles without them', async () => {
  // An update that restarts the Mini 3 s after its start, which ends the
  // mount, and is done after 5 s. The host reads the mount until then, and
  // again from the next mount on.
  const UPDATE = { everySecond: [{ while: { mount: true }, measure: 'mountReadBytesPerSecond', min: 50_000 }],
    changeTakesAtMost: [{ change: { ota: true }, max: 10, pauses: ['mountReadBytesPerSecond'] }, { change: { ota: false }, max: 2 },
      { change: { mount: true }, max: 7, pauses: ['mountReadBytesPerSecond'] }, { change: { mount: false }, max: 3 }] };
  const list = [{ state: { video: true, mount: true, ota: true }, holdSeconds: 0 }, { state: { video: true, mount: true }, holdSeconds: 0 }];
  const updated = async ends => {
    const calls = [];
    let reading = true;
    let bytesRead = 0;
    const switches = timedSwitches(calls, { works: { ota: (ms, on) => {
      if (on && ms >= 3000) reading = false;
      return !on || ms >= 5000;
    } } });
    if (ends) switches.ota.ends = ends;
    const mountOn = switches.mount.on;
    switches.mount.on = async () => { reading = true; await mountOn(); };
    const loads = (at, state) => {
      if (reading) bytesRead += 100_000;
      return state.mount ? { mount: reading ? { bytesRead } : null } : {};
    };
    const result = await driveOnFakeClock(list, switches, UPDATE, [], { state: { video: true, mount: true }, fps: () => 30, loads });
    return { ...result, calls, verdict: judge(result.recording, UPDATE) };
  };
  const ended = await updated(['mount']);
  assert.deepEqual(ended.calls, ['ota on', 'mount off', 'ota off', 'mount on']);
  // The mount's row is written first, with what ended it; the update's row
  // has the state from its start and settles when the mount is off.
  assert.deepEqual(ended.rows.map(row => [row.change, row.endedBy, row.seconds]),
    [[{ mount: false }, 'ota', 0], [{ ota: true }, undefined, 5], [{ ota: false }, undefined, 0], [{ mount: true }, undefined, 0]]);
  assert.deepEqual(ended.rows[1].state, { video: true, mount: true, ota: true });
  assert.equal(changeLine(ended.rows[0], 3), 'mount off: ended by ota on, off after 0.0 s');
  assert.deepEqual([ended.verdict.result, ended.verdict.failures], ['pass', []]);
  // A mount that stays in the state after the update that ended it is
  // judged as a mount that the host does not read.
  const stays = await updated(null);
  assert.deepEqual(stays.calls, ['ota on', 'ota off']);
  assert.deepEqual([stays.rows[0].seconds, stays.rows[0].unsettled], [5 + SETTLE_CAP_SECONDS, 'mountReadBytesPerSecond is 0']);
  assert.equal(stays.verdict.result, 'fail');
  // An ended mount that is off after 4 s, over the unmount's 3 s: the
  // report's line has the time of its switch and what ended it.
  const slowOff = timedSwitches([]);
  slowOff.ota.ends = ['mount'];
  const mountOff = slowOff.mount.off;
  slowOff.mount.off = async () => { await new Promise(resolve => setTimeout(resolve, 4000)); await mountOff(); };
  const slow = await driveOnFakeClock(list, slowOff, UPDATE, [], { state: { video: true, mount: true }, fps: () => 30 });
  assert.match(report(judge(slow.recording, UPDATE), T0), /^FAIL {2}mount off ended by ota on, off after 4\.0 s, limit 3 s$/m);
  // An ended variable whose switch fails ends the walk, and the recording
  // has the row of the update after the row with the error.
  const calls = [];
  const switches = timedSwitches(calls);
  switches.ota.ends = ['mount'];
  switches.mount.off = async () => { throw new Error('the host does not answer'); };
  const failed = await driveOnFakeClock(list, switches, UPDATE, [], { state: { video: true, mount: true }, fps: () => 30 });
  assert.equal(failed.completed, false);
  assert.deepEqual(failed.rows.map(row => [row.change, row.endedBy, row.error]),
    [[{ mount: false }, 'ota', 'the host does not answer'], [{ ota: true }, undefined, null]]);
});

test('the judge works out each settling time from the samples with its own rules, up to the start of the next change', async () => {
  // After the unmount the frame rate is 10 in the sample at 0.7 s, 20 at
  // 1.7 s and 25 up to 7.7 s, and 30 before and after. The walk's rule
  // wants 22 frames: the state settled after 1.7 s, the driver saw that at
  // 3.7 s, and the mount came 2 s later, at 5.75 s.
  const fps = at => (at <= 0 || at >= 8 ? 30 : at < 1 ? 10 : at < 2 ? 20 : 25);
  const criteria = min => ({ everySecond: [{ ...FPS_26, min, failAfterSeconds: 1 }],
    changeTakesAtMost: [{ change: { mount: false }, max: 0.5 }, { change: { mount: true }, max: 7 }] });
  const list = [{ state: { video: true }, holdSeconds: 2 }, { state: { video: true, mount: true }, holdSeconds: 0 }];
  const walk = await driveOnFakeClock(list, timedSwitches([]), criteria(22), [], { state: { video: true, mount: true }, fps });
  const unmounted = walk.rows.find(row => row.change.mount === false);
  assert.deepEqual([unmounted.seconds, walk.rows.find(row => row.change.mount).startedAt - T0], [1.7, 5750]);
  const judged = min => judge(walk.recording, criteria(min));
  const lines = min => report(judged(min), T0).split('\n').filter(line => /^FAIL {2}mount off/.test(line));
  // The walk's rules give the time in the row.
  assert.deepEqual(lines(22), ['FAIL  mount off settled after 1.7 s, limit 0.5 s']);
  assert.deepEqual(judged(22).changes.map(row => [row.change, row.seconds]), [[{ mount: false }, 1.7], [{ mount: true }, 0]]);
  // A rule that wants 15 frames: 20 at 1.7 s is normal, and the state settled after 0.7 s.
  assert.deepEqual(lines(15), ['FAIL  mount off settled after 0.7 s, limit 0.5 s']);
  // A rule that wants 26 frames: 25 is not normal up to the mount.
  assert.deepEqual(lines(26), ['FAIL  mount off did not settle before the next change, limit 0.5 s: framesPerSecond is 25']);
  // Judged again, the recording cannot tell how long that state takes to
  // settle with that rule: the walk found it settled and moved on.
  const again = judge(walk.recording, criteria(26), [], { again: true });
  assert.deepEqual([again.failures.filter(f => f.change?.change.mount === false), again.walkAgain.map(w => [w.change.change, w.movedOn - T0])],
    [[], [[{ mount: false }, 5750]]]);
  assert.match(report(again, T0), /^WALK AGAIN {2}mount off did not settle before the next change under the current rules; the walk moved on at 6 s$/m);
  // A state that the walk itself found not settled fails.
  const unsettledRow = walk.recording.map(row => (row === unmounted ? { ...row, unsettled: 'framesPerSecond is 25' } : row));
  const unsettled = judge(unsettledRow, criteria(26), [], { again: true });
  assert.deepEqual([unsettled.walkAgain, unsettled.failures.filter(f => f.unsettled).map(f => f.settleEnd)], [[], ['before the next change']]);
});

test('the judge lets a state settle from the time at which the variables that its change ended are off', async () => {
  // The update is done after 5 s and ends the mount, which is off 4 s
  // later. The frame rate is 10 in the samples at 5.7 s to 7.7 s, while the
  // mount goes off: those seconds do not count, and the update settled
  // after 9 s.
  const criteria = { everySecond: [{ ...FPS_26, failAfterSeconds: 1 }],
    changeTakesAtMost: [{ change: { ota: true }, max: 8 }, { change: { mount: false }, max: 5 }, { change: { ota: false }, max: 2 }] };
  const switches = timedSwitches([], { works: { ota: (ms, on) => !on || ms >= 5000 } });
  switches.ota.ends = ['mount'];
  const mountOff = switches.mount.off;
  switches.mount.off = async () => { await new Promise(resolve => setTimeout(resolve, 4000)); await mountOff(); };
  const list = [{ state: { video: true, mount: true, ota: true }, holdSeconds: 0 }, { state: { video: true, mount: true }, holdSeconds: 0 }];
  const walk = await driveOnFakeClock(list, switches, criteria, [], { state: { video: true, mount: true }, fps: low(5, 6, 7) });
  assert.deepEqual(walk.rows.map(row => [row.change, row.endedBy, row.seconds]),
    [[{ mount: false }, 'ota', 4], [{ ota: true }, undefined, 9], [{ ota: false }, undefined, 0], [{ mount: true }, undefined, 0]]);
  assert.deepEqual(report(judge(walk.recording, criteria), T0).split('\n').filter(line => /^FAIL {2}(ota|mount)/.test(line)),
    ['FAIL  ota on settled after 9.0 s, limit 8 s']);
});

test('a recording whose change row lacks what the judge needs stops the judge with the name of the missing field', () => {
  const { switchSeconds, ...noSwitch } = change('mount', true, 0, 2);
  assert.throws(() => judge([noSwitch, ...recording(5, { mount: true })], MOUNT_LIMIT),
    /^Error: the recording's row of the change mount on at 1002000 has no "switchSeconds"$/);
});

test('a fault from before a change stays the old state\'s when the new state has another rule for its measure', async () => {
  // Video is at 10 frames per second from the start. The mount at 3 s
  // brings a rule of its own for the frame rate, whose count of seconds
  // begins there: the first of them is no sign that the frame rate is back,
  // and the state is settled when the switch is done.
  const criteria = { everySecond: [FPS_26, { while: { video: true, mount: true }, measure: 'framesPerSecond', min: 20, failAfterSeconds: 2 }],
    changeTakesAtMost: [{ change: { mount: true }, max: 7 }] };
  const list = [{ state: { video: true }, holdSeconds: 3 }, { state: { video: true, mount: true }, holdSeconds: 0 }];
  const result = await driveOnFakeClock(list, timedSwitches([]), criteria, [], { state: { video: true }, fps: () => 10 });
  const row = result.rows.find(one => one.change.mount);
  assert.deepEqual([row.startedAt - T0, row.seconds, row.unsettled, row.waitedFor], [3000, 0, undefined, undefined]);
});

test('a measure that was not normal before a change, and has not been normal since, does not hold up the settling of that change', () => {
  // A second with the measures that are not normal in it; of the two
  // measures of this test, the others are normal, except those in `unknown`.
  const second = (at, measures = [], unknown = []) => ({ t: T0 + at * 1000, notNormal: measures.map(measure => ({ measure, value: 1 })),
    normal: ['freePsramLowWater', 'keysLost'].filter(measure => !measures.includes(measure) && !unknown.includes(measure)) });
  // A low-water mark that fell before the change stays where it is.
  const mark = [-1.3, 0.7, 1.7, 2.7].map(at => second(at, ['freePsramLowWater']));
  assert.deepEqual(settledAt(mark, T0, T0), { settledAt: T0, waitedFor: {}, last: null });
  // The recorder has the mark once in some seconds: the seconds without it
  // do not say that it is back.
  const rare = [second(-2.3, ['freePsramLowWater']), second(-1.3, [], ['freePsramLowWater']), second(0.7, [], ['freePsramLowWater']),
    second(1.7, [], ['freePsramLowWater']), second(2.7, ['freePsramLowWater'])];
  assert.equal(settledAt(rare, T0, T0).settledAt, T0);
  // Keys that were lost before the change, none after it and lost again
  // at 1.7 s are the change's: settled from 1.7 s on.
  const again = [second(-1.3, ['keysLost']), second(0.7), second(1.7, ['keysLost']), second(2.7), second(3.7)];
  assert.deepEqual(settledAt(again, T0, T0), { settledAt: T0 + 1700, waitedFor: { keysLost: { seconds: 1, last: 1 } }, last: 'keysLost is 1' });
  // A measure that the mark's seconds have beside it is the change's too.
  const beside = [second(-1.3, ['freePsramLowWater']), second(0.7, ['freePsramLowWater', 'keysLost']),
    second(1.7, ['freePsramLowWater']), second(2.7, ['freePsramLowWater'])];
  assert.equal(settledAt(beside, T0, T0).settledAt, T0 + 700);
  // A sample that began 0.1 s before the change can have what the change
  // did: keys that are lost from that sample on are the change's.
  const across = [second(-1.1), second(-0.1, ['keysLost']), second(0.9, ['keysLost']), second(1.9), second(2.9), second(3.9)];
  assert.equal(settledAt(across, T0, T0).settledAt, T0 + 900);
  // Two samples do not cover two seconds after a switch at 0.5 s; three do.
  const three = [second(0.7), second(1.7), second(2.7)];
  assert.equal(settledAt(three.slice(0, 2), T0, T0 + 500).settledAt, null);
  assert.equal(settledAt(three, T0, T0 + 500).settledAt, T0 + 500);
});

test('a measure that a change pauses is checked again when the switch is done, also while the state settles, and an entry for a measure during the change applies until then', () => {
  // A mount whose switch is done after 2 s and whose state is settled after
  // 6 s. Video stops in the seconds 1, 3 and 5.
  const criteria = { everySecond: [{ while: { video: true }, measure: 'framesPerSecond', min: 1 }],
    changeTakesAtMost: [{ change: { mount: true }, max: 10, pauses: ['framesPerSecond'] }] };
  const rows = [...recording(10, { video: true, mount: true }, { frozen: [1, 3, 5] }),
    { ...change('mount', true, 0, 6, { state: { video: true } }), switchSeconds: 2 }];
  const failed = known => judge(rows, criteria, known).failures.map(f => f.t - T0);
  assert.deepEqual(failed([]), [3000, 5000]);
  // The entry applies during the switch and for 1 s after it.
  const entry = { issue: 'https://example.test/issues/268', change: { mount: true }, measure: 'framesPerSecond', tolerateSeconds: 1, recoverySeconds: 1 };
  assert.deepEqual(failed([entry]), [5000]);
});

// Every command of a host, with an example of each argument.
const SENDER = {
  url: 'https://releases.test/0.4.3/sender package', sha256: 'ab'.repeat(32), file: 'sender.tar.gz', fingerprint: 'AA:BB:CC:DD',
};
const PAGE_URL = 'http://192.168.1.102:40123/target-page';
const hostCommands = host => [host.watchKeys, host.watchMedia, host.canRead, host.startReader, host.stopReader,
  host.releaseMedia, host.installSender(SENDER), host.startSender, host.stopSender,
  host.checksumsAndRemove(["C:\\Users\\it's me\\combined-load-1.bin", '/home/me/combined-load-2.bin']),
  host.openPage(PAGE_URL), host.closePage, host.fullVolume,
  host.displayMode?.(), host.displayMode?.({ width: 1280, height: 720 })].filter(Boolean);
const UNAMES = ['Linux', 'Darwin', 'MINGW64_NT-10.0-26300'];

test('a sender of one\'s own needs both its URL and its SHA-256, and without either the run takes the newest release', () => {
  const sum = 'AB'.repeat(32);
  assert.equal(senderOverride({}), null);
  assert.equal(senderOverride({ JETKVM_SENDER_URL: '', JETKVM_SENDER_SHA256: '' }), null);
  assert.deepEqual(senderOverride({ JETKVM_SENDER_URL: 'http://192.168.1.102:8080/pkg/My%2BSender_1.0.AppImage', JETKVM_SENDER_SHA256: sum }),
    { url: 'http://192.168.1.102:8080/pkg/My%2BSender_1.0.AppImage', sha256: 'ab'.repeat(32), file: 'My+Sender_1.0.AppImage' });
  // The name goes into a shell command on the host.
  for (const name of ['sender-%24(id).AppImage', 'a%22b.AppImage', 'a%60id%60.AppImage', 'My%20Sender.AppImage']) {
    assert.throws(() => senderOverride({ JETKVM_SENDER_URL: `http://h/${name}`, JETKVM_SENDER_SHA256: sum }), /may have only letters, digits/);
  }
  assert.throws(() => senderOverride({ JETKVM_SENDER_URL: 'http://h/s.AppImage' }), /JETKVM_SENDER_URL and JETKVM_SENDER_SHA256 go together/);
  assert.throws(() => senderOverride({ JETKVM_SENDER_SHA256: sum }), /go together/);
  assert.throws(() => senderOverride({ JETKVM_SENDER_URL: 'http://h/s.AppImage', JETKVM_SENDER_SHA256: 'ab'.repeat(31) }), /64 hex digits/);
  assert.throws(() => senderOverride({ JETKVM_SENDER_URL: 'http://h/s.AppImage', JETKVM_SENDER_SHA256: 'xy'.repeat(32) }), /64 hex digits/);
  assert.throws(() => senderOverride({ JETKVM_SENDER_URL: 'h/s.AppImage', JETKVM_SENDER_SHA256: sum }), /is not a URL/);
  assert.throws(() => senderOverride({ JETKVM_SENDER_URL: 'file:///tmp/s.AppImage', JETKVM_SENDER_SHA256: sum }), /http or https URL/);
  assert.throws(() => senderOverride({ JETKVM_SENDER_URL: 'http://h/pkg/', JETKVM_SENDER_SHA256: sum }), /http or https URL/);
});

test('a sender of one\'s own must be the package type of the host', () => {
  const sum = 'ab'.repeat(32);
  const packages = { Linux: 'a.AppImage', Darwin: 'a.app.tar.gz', 'MINGW64_NT-10.0-26300': 'a-setup.exe' };
  for (const uname of UNAMES) {
    for (const [other, file] of Object.entries(packages)) {
      const sender = senderOverride({ JETKVM_SENDER_URL: `http://h/${file}`, JETKVM_SENDER_SHA256: sum });
      if (other === uname) checkSenderType(sender, hostFor(uname), 'me@host');
      else assert.throws(() => checkSenderType(sender, hostFor(uname), 'me@host'), new RegExp(`names ${file.replace(/\./g, '\\.')}, but me@host installs a ${hostFor(uname).senderPackage} package`));
    }
    checkSenderType(null, hostFor(uname), 'me@host');
  }
});

test('osServices installs the newest release, or a sender of one\'s own without reading releases.json, and records which', async () => {
  const releases = { releases: [
    { version: '0.4.3', packages: { 'macos-aarch64': { path: '0.4.3/old.app.tar.gz', sha256: '11'.repeat(32) } } },
    { version: '0.4.4', packages: { 'macos-aarch64': { path: '0.4.4/IP-KVM-OS-Services_0.4.4_macos-aarch64.app.tar.gz', sha256: '22'.repeat(32) } } },
  ] };
  const mac = hostFor('Darwin');
  assert.deepEqual(await senderToInstall(null, mac, async () => releases), {
    url: `${RELEASES_URL}/0.4.4/IP-KVM-OS-Services_0.4.4_macos-aarch64.app.tar.gz`, sha256: '22'.repeat(32),
    file: 'IP-KVM-OS-Services_0.4.4_macos-aarch64.app.tar.gz', recorded: '0.4.4',
  });
  await assert.rejects(senderToInstall(null, hostFor('Linux'), async () => releases), /release 0\.4\.4 of the sender has no package linux-x86_64-appimage/);
  const sender = senderOverride({ JETKVM_SENDER_URL: 'http://192.168.1.102:8080/s.app.tar.gz', JETKVM_SENDER_SHA256: '3c'.repeat(32) });
  assert.deepEqual(await senderToInstall(sender, mac, async () => assert.fail('releases.json is read')), {
    ...sender, recorded: `custom ${'3c'.repeat(6)} http://192.168.1.102:8080/s.app.tar.gz`,
  });
  // A presigned URL or one with a login keeps its credentials out of the recording.
  const signed = senderOverride({ JETKVM_SENDER_URL: 'https://me:secret@bucket.test/b/s.app.tar.gz?X-Amz-Signature=abc#x', JETKVM_SENDER_SHA256: '3c'.repeat(32) });
  const { recorded, url } = await senderToInstall(signed, mac, async () => assert.fail('releases.json is read'));
  assert.equal(recorded, `custom ${'3c'.repeat(6)} https://bucket.test/b/s.app.tar.gz`);
  assert.equal(url, signed.url);
});

test('the other display mode is 1280x720, a host that has it already is refused, and a host that prints no mode is an error', () => {
  const full = { width: 1920, height: 1080 };
  const small = { width: 1280, height: 720 };
  // The mode before keeps its rate, for the way back; the other mode has none.
  assert.deepEqual(otherMode(parseSize('1920 1080 29.97\n')), { from: { ...full, hz: 29.97 }, to: small });
  assert.throws(() => otherMode(parseSize('1280 720 60\n')), /the mode that hostModeChange sets/);
  assert.throws(() => parseSize('1920 1080\n'), /did not print its display mode/);
  assert.throws(() => parseSize('\n'), /did not print its display mode/);
});

test('a Windows host gets a display mode script that Python can compile, with the wanted size as its arguments, and a macOS host has none', () => {
  // Stand-ins for the host's programs: cygpath gives the path back, and
  // python compiles the script that the command wrote and notes its arguments.
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const stub = (name, script) => fs.writeFileSync(path.join(directory, name), `#!/bin/sh\n${script}\n`, { mode: 0o755 });
  stub('cygpath', 'echo "$2"');
  stub('python', 'shift; python3 -m py_compile "$1" && shift && echo "$@"');
  try {
    const run = size => spawnSync('sh', ['-c', hostFor('MINGW64_NT-10.0').displayMode(size)],
      { encoding: 'utf8', env: { PATH: `${directory}:/usr/bin:/bin`, HOME: directory } });
    const read = run();
    assert.equal(read.status, 0, read.stderr);
    assert.equal(read.stdout, 'relay\n');
    assert.equal(run({ width: 1280, height: 720 }).stdout, 'relay 1280 720\n');
    assert.equal(run({ width: 1920, height: 1080, hz: 60 }).stdout, 'relay 1920 1080 60\n');
    assert.equal(hostFor('Darwin').displayMode, undefined);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test('a Linux host sets a display mode of the wanted size at the rate nearest to its own, as a temporary configuration, and a host with two displays or a scaled one is refused', () => {
  // A stand-in for Mutter behind PyGObject: it answers GetCurrentState with
  // the Mini's monitor, as a bench host has it, and notes every other call.
  // HOST=two adds a second display in use, and HOST=scaled has the one at 200 %.
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  fs.writeFileSync(path.join(directory, 'systemctl'), '#!/bin/sh\ntrue\n', { mode: 0o755 });
  fs.mkdirSync(path.join(directory, 'gi'));
  fs.writeFileSync(path.join(directory, 'gi', '__init__.py'), '');
  fs.writeFileSync(path.join(directory, 'gi', 'repository.py'), `
import json, os
monitor = ("HDMI-A-0", "JET", "JetKVM v1", "P4TC358743")
mode = lambda w, h, hz, **flags: ("%dx%d@%.3f" % (w, h, hz), w, h, hz, 1.0, [1.0], flags)
modes = [mode(1280, 720, 60.0), mode(1280, 720, 29.97), mode(1280, 720, 30.0), mode(1680, 1050, 30.0),
         mode(1920, 1080, 29.97), mode(1920, 1080, 30.0, **{"is-current": True, "is-preferred": True})]
other = ("DP-1", "DEL", "DELL U2720Q", "1")
others = [mode(3840, 2160, 60.0, **{"is-current": True}), mode(1280, 720, 60.0)]
host = os.environ.get("HOST")
class Answer:
    def unpack(self):
        return (7, [(other, others, {}), (monitor, modes, {})],
                [(0, 0, 2.0 if host == "scaled" else 1.0, 0, True, [monitor], {})] +
                ([(1920, 0, 1.0, 0, False, [other], {})] if host == "two" else []), {})
class Bus:
    def call_sync(self, name, path, interface, method, arguments, *rest):
        if method == "GetCurrentState":
            return Answer()
        with open(os.path.join(os.environ["HOME"], "asked"), "a") as note:
            note.write(json.dumps([method, arguments.signature, arguments.value]) + "\\n")
class Gio:
    class BusType: SESSION = 1
    class DBusCallFlags: NONE = 0
    bus_get_sync = staticmethod(lambda *a: Bus())
class Variant:
    def __init__(self, signature, value):
        self.signature, self.value = signature, value
class GLib:
    Variant = Variant
`);
  const asked = path.join(directory, 'asked');
  const run = (size, host = 'one') => {
    fs.rmSync(asked, { force: true });
    const result = spawnSync('sh', ['-c', hostFor('Linux').displayMode(size)],
      { encoding: 'utf8', env: { PATH: `${directory}:/usr/bin:/bin`, HOME: directory, PYTHONPATH: directory, HOST: host } });
    return { ...result, asked: fs.existsSync(asked) ? fs.readFileSync(asked, 'utf8').trim().split('\n').map(line => JSON.parse(line)) : [] };
  };
  try {
    // Without a size it prints the mode and changes nothing.
    const read = run();
    assert.equal(read.status, 0, read.stderr);
    assert.equal(read.stdout, '1920 1080 30.0\n');
    assert.deepEqual(read.asked, []);
    // With a size: the mode before, then one call. Its second argument is the
    // method, and 1 is "temporary": 2, "persistent", stores the mode, and a
    // stored mode that nobody confirms can end the host's desktop session at
    // the next monitor hotplug.
    const set = run({ width: 1280, height: 720 });
    assert.equal(set.status, 0, set.stderr);
    assert.equal(set.stdout, '1920 1080 30.0\n');
    assert.deepEqual(set.asked, [['ApplyMonitorsConfig', '(uua(iiduba(ssa{sv}))a{sv})',
      [7, 1, [[0, 0, 1.0, 0, true, [['HDMI-A-0', '1280x720@30.000', {}]]]], {}]]]);
    // With a rate, the mode of that size nearest to the rate and not to the
    // current one: the way back to a mode that had another rate.
    assert.deepEqual(run({ width: 1280, height: 720, hz: 59.9 }).asked[0][2][2][0][5], [['HDMI-A-0', '1280x720@60.000', {}]]);
    // A size that the monitor does not offer fails with its name, and nothing is set.
    const none = run({ width: 3840, height: 2160 });
    assert.notEqual(none.status, 0);
    assert.match(none.stderr, /HDMI-A-0 offers no 3840x2160/);
    assert.deepEqual(none.asked, []);
    // A second display would need a new layout, and a scale of 200 % is not
    // valid for 1280x720: such a host is refused when its mode is read,
    // which is before the list, and nothing is set.
    for (const [host, has] of [['two', /HDMI-A-0 at 100 %, DP-1 at 100 %/], ['scaled', /HDMI-A-0 at 200 %/]]) {
      for (const size of [undefined, { width: 1280, height: 720 }]) {
        const refused = run(size, host);
        assert.notEqual(refused.status, 0, host);
        assert.match(refused.stderr, /needs a host with one display in use, at a scale of 100 %/);
        assert.match(refused.stderr, has);
        assert.deepEqual([refused.stdout, refused.asked], ['', []], host);
      }
    }
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test('a Linux host sets the volume of the Mini\'s audio output to full, and of no other output', () => {
  // Stand-ins for PipeWire's programs: pw-dump prints the host's objects, and wpctl notes what it is asked.
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const stub = (name, script) => fs.writeFileSync(path.join(directory, name), `#!/bin/sh\n${script}\n`, { mode: 0o755 });
  stub('systemctl', 'true');
  stub('wpctl', 'echo "$@" >> "$HOME/asked"');
  const sink = (id, name, description) => ({ id, type: 'PipeWire:Interface:Node',
    info: { props: { 'media.class': 'Audio/Sink', 'node.name': name, 'node.description': description } } });
  const others = [
    { id: 30, type: 'PipeWire:Interface:Client', info: { props: { 'application.name': 'USB Emulation Device viewer' } } },
    { id: 31, type: 'PipeWire:Interface:Link', info: null },
    sink(58, 'alsa_output.usb-CSCTEK_USB_Audio_and_HID-00.analog-stereo', 'USB Audio and HID Analog Stereo'),
    // The Mini's microphone side would be a source, which is no output.
    { ...sink(90, 'alsa_input.usb-JetKVM_USB_Emulation_Device-04', 'USB Emulation Device'), info: { props: { 'media.class': 'Audio/Source', 'node.name': 'alsa_input.usb-JetKVM' } } },
  ];
  const run = objects => {
    fs.writeFileSync(path.join(directory, 'objects.json'), JSON.stringify(objects));
    stub('pw-dump', 'cat "$HOME/objects.json"');
    fs.rmSync(path.join(directory, 'asked'), { force: true });
    const result = spawnSync('sh', ['-c', hostFor('Linux').fullVolume],
      { encoding: 'utf8', env: { PATH: `${directory}:/usr/bin:/bin`, HOME: directory } });
    assert.equal(result.status, 0, result.stderr);
    return fs.existsSync(path.join(directory, 'asked')) ? fs.readFileSync(path.join(directory, 'asked'), 'utf8') : '';
  };
  const mini = sink(111, 'alsa_output.usb-JetKVM_USB_Emulation_Device-04.analog-stereo', 'USB Emulation Device Analog Stereo');
  assert.equal(run([...others, mini]), 'set-volume 111 1.0\n');
  // A host that the Mini is not on has nothing to set, and that is no error.
  assert.equal(run(others), '');
  // macOS has no volume for this output, and Windows has it at full.
  assert.equal(hostFor('Darwin').fullVolume, undefined);
  assert.equal(hostFor('MINGW64_NT-10.0-26300').fullVolume, undefined);
});

test('the host is chosen by its uname, and a host that the tool cannot drive is refused', () => {
  assert.deepEqual(UNAMES.map(uname => hostFor(`${uname}\n`).system), ['linux', 'macos', 'windows']);
  assert.equal(hostFor('MSYS_NT-10.0-22631\n').system, 'windows');
  assert.throws(() => hostFor('FreeBSD\n'), /FreeBSD/);
});

test('each host takes the bytes read from its own watch line', () => {
  // Linux: /sys/block/<device>/stat, whose third number is in 512-byte sectors.
  const stat = 'rom /dev/sr0      407        0    16384       52        0        0        0        0        0       96       52';
  assert.deepEqual(hostFor('Linux').medium(stat.split(/\s+/)), { type: 'rom', device: '/dev/sr0', bytesRead: 16384 * 512 });
  // With the kernel's count of failed commands, which is hexadecimal; a device without the count has "errors=".
  assert.deepEqual(hostFor('Linux').medium(`${stat} errors=0x1a`.split(/\s+/)),
    { type: 'rom', device: '/dev/sr0', bytesRead: 16384 * 512, readErrors: 26 });
  assert.deepEqual(hostFor('Linux').medium(`${stat} errors=`.split(/\s+/)), { type: 'rom', device: '/dev/sr0', bytesRead: 16384 * 512 });
  assert.deepEqual(hostFor('Darwin').medium('disk /dev/rdisk4 8388608'.split(/\s+/)),
    { type: 'disk', device: '/dev/rdisk4', bytesRead: 8388608 });
  assert.deepEqual(hostFor('MINGW64_NT-10.0').medium('rom //./D: 00000000000002883584'.split(/\s+/)),
    { type: 'rom', device: '//./D:', bytesRead: 2883584 });
});

test('every host command is valid sh', () => {
  for (const uname of UNAMES) {
    for (const command of hostCommands(hostFor(uname))) {
      const result = spawnSync('sh', ['-n', '-c', command], { encoding: 'utf8' });
      assert.equal(result.status, 0, `${uname}: ${result.stderr}\n${command.slice(0, 200)}`);
    }
  }
});

test('a Linux host is left without the mount of a stopped sender, and a start and an install first remove the mounts that nothing serves', () => {
  // Stand-ins for the host's programs: findmnt prints the mounts, pgrep knows
  // one running AppImage, and fusermount3 notes what it is asked to unmount.
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const stub = (name, script) => fs.writeFileSync(path.join(directory, name), `#!/bin/sh\n${script}\n`, { mode: 0o755 });
  stub('systemctl', 'echo "systemctl $*" >> "$HOME/asked"');
  stub('fusermount3', 'echo "fusermount3 $*" >> "$HOME/asked"');
  // The name of a process is the first 15 characters of its program's file name.
  stub('pgrep', '[ "$1" = -x ] && [ "$2" = "IP-KVM-OS-Servi" ] && [ -f "$HOME/sender-runs" ]');
  // Mount points as the AppImage makes them. The mounts are stand-ins too: plain directories.
  const mountPoint = () => fs.mkdtempSync('/tmp/.mount_IP-KVM');
  const [left, other, anotherUsers] = [mountPoint(), mountPoint(), mountPoint()];
  const foreign = fs.mkdtempSync('/tmp/.mount_Other');
  const appImage = 'IP-KVM-OS-Services_0.4.3_linux-x86_64.AppImage';
  const mine = `ro,nosuid,nodev,relatime,user_id=${process.getuid()},group_id=${process.getgid()}`;
  const mounts = lines => stub('findmnt', lines.map(line => `echo '${line}'`).join('\n'));
  mounts(['tmpfs /tmp rw,nosuid', `${appImage} ${left} ${mine}`, `${appImage} ${other} ${mine}`,
    `${appImage} ${anotherUsers} ro,nosuid,nodev,relatime,user_id=${process.getuid() + 1},group_id=7`,
    `Other-App.AppImage ${foreign} ${mine}`, '/dev/sda1 / rw,relatime']);
  const run = command => {
    fs.rmSync(path.join(directory, 'asked'), { force: true });
    const result = spawnSync('sh', ['-c', command], { encoding: 'utf8', env: { PATH: `${directory}:/usr/bin:/bin`, HOME: directory } });
    assert.equal(result.status, 0, result.stderr);
    return fs.readFileSync(path.join(directory, 'asked'), 'utf8').trim().split('\n')
      .filter(line => !line.includes('show-environment'));
  };
  try {
    const { installSender, startSender, stopSender } = hostFor('Linux');
    // While a process of the AppImage runs, its mounts are in use: a stop of the unit leaves them.
    fs.writeFileSync(path.join(directory, 'sender-runs'), '');
    assert.deepEqual(run(stopSender), ['systemctl --user stop ipkvm-os-services.service']);
    assert.ok(fs.existsSync(left) && fs.existsSync(other));
    // With no such process, the stop unmounts this user's mounts of the
    // sender and removes their directories, and no other mount.
    fs.rmSync(path.join(directory, 'sender-runs'));
    assert.deepEqual(run(stopSender), ['systemctl --user stop ipkvm-os-services.service',
      `fusermount3 -u ${left}`, `fusermount3 -u ${other}`]);
    assert.ok(!fs.existsSync(left) && !fs.existsSync(other) && fs.existsSync(anotherUsers) && fs.existsSync(foreign));
    // A start does the same before it starts the unit.
    fs.mkdirSync(left);
    mounts([`${appImage} ${left} ${mine}`]);
    assert.deepEqual(run(startSender), [`fusermount3 -u ${left}`,
      'systemctl --user reset-failed ipkvm-os-services.service', 'systemctl --user start ipkvm-os-services.service']);
    assert.ok(!fs.existsSync(left));
    // An install starts the AppImage three times, each of which needs a free
    // mount, so it unmounts first. A mount that cannot be unmounted and a
    // directory that cannot be removed do not end the install, which ends at
    // any other failure.
    fs.mkdirSync(left);
    fs.writeFileSync(path.join(left, 'in use'), '');
    stub('fusermount3', 'echo "fusermount3 $*" >> "$HOME/asked"; exit 1');
    stub('sha256sum', 'true');
    stub('sudo', 'shift; exec "$@"');
    const senderDirectory = path.join(directory, '.local/share/jetkvm-combined-load');
    fs.mkdirSync(senderDirectory, { recursive: true });
    fs.writeFileSync(path.join(senderDirectory, appImage), '#!/bin/sh\necho "AppImage $*" >> "$HOME/asked"\n', { mode: 0o755 });
    assert.deepEqual(run(installSender({ ...SENDER, file: appImage })), [
      `fusermount3 -u ${left}`, `fusermount3 -uz ${left}`,
      'AppImage --approve-kvm AA:BB:CC:DD', 'AppImage --install-linux-access', 'AppImage --install-service',
      'systemctl --user stop ipkvm-os-services.service', `fusermount3 -u ${left}`, `fusermount3 -uz ${left}`]);
  } finally {
    for (const made of [left, other, anotherUsers, foreign]) fs.rmSync(made, { recursive: true, force: true });
  }
});

test('the Python script of each watch reaches Python whole', () => {
  // Stand-ins for the host's programs: python compiles what it is given to
  // run, from -c or from a file, and notes that it did.
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const stub = (name, script) => fs.writeFileSync(path.join(directory, name), `#!/bin/sh\n${script}\n`, { mode: 0o755 });
  const python = spawnSync('sh', ['-c', 'command -v python3'], { encoding: 'utf8' }).stdout.trim();
  stub('python3',
    'while [ $# -gt 0 ]; do case "$1" in -u) shift;; -c) printf %s "$2" > "$HOME/source"; break;; ' +
    '*) cp "$1" "$HOME/source"; break;; esac; done\n' +
    `exec ${python} -c 'import sys; compile(open(sys.argv[1]).read(), "script", "exec"); print("compiled")' "$HOME/source" >> "$HOME/compiled"`);
  fs.symlinkSync('python3', path.join(directory, 'python'));
  stub('sudo', 'shift; exec "$@"');
  stub('cygpath', 'shift; printf %s "$1"');
  const env = { PATH: `${directory}:/usr/bin:/bin`, HOME: directory };
  const watches = [['Linux', 'watchKeys'], ['Darwin', 'watchMedia'], ['MINGW64_NT-10.0', 'watchKeys'], ['MINGW64_NT-10.0', 'watchMedia']];
  for (const [uname, watch] of watches) {
    const result = spawnSync('sh', ['-c', hostFor(uname)[watch]], { encoding: 'utf8', env });
    assert.equal(result.status, 0, `${uname} ${watch}: ${result.stderr}`);
  }
  // The script that shows the page on Windows, which has the page's URL in it.
  const show = spawnSync('sh', ['-c', hostFor('MINGW64_NT-10.0').openPage(PAGE_URL)], { encoding: 'utf8', env });
  assert.equal(show.status, 0, show.stderr);
  assert.equal(fs.readFileSync(path.join(directory, 'compiled'), 'utf8'), 'compiled\n'.repeat(watches.length + 1));
  assert.ok(fs.readFileSync(path.join(directory, 'source'), 'utf8').includes(`"--app=${PAGE_URL}"`));
});

test('each host opens the page at the URL it is given, and a URL cannot end the command', () => {
  for (const uname of UNAMES) {
    assert.ok(hostFor(uname).openPage(PAGE_URL).includes(PAGE_URL), uname);
    // The browser gives its audio outputs to the page of this server only.
    assert.ok(hostFor(uname).openPage(PAGE_URL).includes('--unsafely-treat-insecure-origin-as-secure=http://192.168.1.102:40123'), uname);
    const result = spawnSync('sh', ['-n', '-c', hostFor(uname).openPage("http://host/it's")], { encoding: 'utf8' });
    assert.equal(result.status, 0, `${uname}: ${result.stderr}`);
  }
});

test('the tool serves the target page, which loads nothing else, and keeps the page\'s newest report', async () => {
  const server = await serve('127.0.0.1', null);
  try {
    const response = await fetch(`${server.base}/target-page`);
    assert.match(response.headers.get('content-type'), /^text\/html/);
    const html = await response.text();
    assert.match(html, /<canvas/);
    // No script, style sheet, font or picture from another file or server.
    assert.doesNotMatch(html, /\b(src|href)\s*=|url\(|@import|https?:\/\//);
    // What it fetches is on the tool's server: its report and the tool's wish for the sound.
    assert.deepEqual(html.match(/fetch\(\s*[^\s,)]*/g), ["fetch('/target-page/report'", "fetch('/target-page/sound?wanted='"]);

    assert.equal(server.served.pageReport, null);
    const reports = [];
    server.served.onPageReport = report => reports.push(report.framesPerSecond);
    const send = framesPerSecond => fetch(`${server.base}/target-page/report`,
      { method: 'POST', body: JSON.stringify({ framesPerSecond, width: 1920 }) }).then(answer => answer.json());
    // The answer keeps the page silent until the tool asks for the sound.
    assert.deepEqual(await send(59), { sound: false, noise: false });
    server.wantSound(true);
    assert.deepEqual(await send(60), { sound: true, noise: false });
    const { at, ...report } = server.served.pageReport;
    assert.deepEqual(report, { framesPerSecond: 60, width: 1920 });
    assert.ok(Date.now() - at < 5000);
    assert.deepEqual(reports, [59, 60]);
    // The image of the URL mounts is still at every other path.
    assert.equal((await fetch(`${server.base}/combined-load.iso`, { headers: { Range: 'bytes=0-9' } })).status, 206);
  } finally {
    server.close();
  }
});

// What the target page says its picture is: the script block that the page
// keeps apart from its drawing.
const pageContent = () => {
  const html = fs.readFileSync(path.join(here, '..', 'target_page.html'), 'utf8');
  const [, source] = /<script id="content">([\s\S]*?)<\/script>/.exec(html);
  return new Function(`${source}; return { boxes, miniAudioOutput, noisePixels };`)();
};

test('the target page\'s noise picture is the same in every run, opaque, and has detail in every part of it', () => {
  const { noisePixels } = pageContent();
  const [width, height] = [96, 64];
  const pixels = noisePixels(width, height);
  assert.equal(pixels.length, width * height * 4);
  assert.equal(sha256(Buffer.from(pixels.buffer)), sha256(Buffer.from(noisePixels(width, height).buffer)));
  // A larger picture has the same pixels at the same places.
  const larger = noisePixels(width * 2, height);
  for (const [x, y] of [[0, 0], [17, 5], [95, 63]]) {
    assert.deepEqual([...larger.subarray((y * width * 2 + x) * 4, (y * width * 2 + x) * 4 + 4)], [...pixels.subarray((y * width + x) * 4, (y * width + x) * 4 + 4)]);
  }
  // Every 16 by 16 block has many different values of each colour, and every pixel is opaque.
  for (let by = 0; by < height; by += 16) {
    for (let bx = 0; bx < width; bx += 16) {
      for (const channel of [0, 1, 2]) {
        const values = new Set();
        for (let y = by; y < by + 16; y++) for (let x = bx; x < bx + 16; x++) values.add(pixels[(y * width + x) * 4 + channel]);
        assert.ok(values.size >= 24, `block ${bx},${by} channel ${channel}: ${values.size} values`);
      }
    }
  }
  assert.ok(pixels.every((value, index) => index % 4 !== 3 || value === 255));
});

test('the target page takes only the Mini\'s own audio output, and none when the browser names no such output', () => {
  const { miniAudioOutput } = pageContent();
  const output = (deviceId, label) => ({ kind: 'audiooutput', deviceId, label });
  const speakers = output('a1', 'Mac mini Speakers');
  const earbuds = output('a2', 'Adam’s AirPods Pro 3');
  // The names that macOS, PipeWire and Windows give the Mini's output.
  for (const label of ['USB Emulation Device (399b:0001)', 'USB Emulation Device Analog Stereo', 'Speakers (JetKVM USB Emulation Device)']) {
    assert.equal(miniAudioOutput([speakers, earbuds, output('m1', label)])?.deviceId, 'm1', label);
  }
  assert.equal(miniAudioOutput([speakers, earbuds]), null);
  assert.equal(miniAudioOutput([]), null);
  // "default" follows the computer's selected output, also when that is the Mini's now.
  assert.equal(miniAudioOutput([output('default', 'Default - USB Emulation Device'), output('communications', 'Communications - USB Emulation Device'), speakers]), null);
  // The Mini's microphone side is no output.
  assert.equal(miniAudioOutput([{ kind: 'audioinput', deviceId: 'i1', label: 'USB Emulation Device' }]), null);
  // A browser that keeps the names to itself: every label is empty.
  assert.equal(miniAudioOutput([output('', ''), output('x', '')]), null);
});

test('the moving part of the target page changes with every frame and stays inside its 960 by 540 pixels', () => {
  const { boxes } = pageContent();
  // A screen capture that sends a frame only when the screen changes gets a
  // new picture with every frame: every box moves, and stays in the part.
  for (let n = 1; n <= 2000; n++) {
    const [now, before] = [boxes(n), boxes(n - 1)];
    assert.equal(now.length, 8);
    now.forEach((box, i) => {
      assert.notEqual(box.x, before[i].x, `frame ${n}, box ${i}`);
      assert.ok(box.x >= 0 && box.x + box.width <= 960 && box.y >= 0 && box.y + box.height <= 540, `frame ${n}, box ${i}`);
    });
  }
});

// A directory with stand-ins for the programs of a Mac whose registry has
// five media of the Mini:
//   disk7  a disc
//   disk8  a disk with a medium that nothing else reads
//   disk9  a disk whose medium is gone: a read of it does not return for
//          4 s, and its eject not for 30 s
//   disk5  a disk that a reader reads: its driver's count grows by 100000
//          with each look at the registry
//   disk6  a disk with a medium whose read takes 1.2 s
// As on a Mac, each driver's count has the bytes of every dd that has read
// its medium. Each stand-in notes its arguments in a file with its name.
function macWithMedia() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const stub = (name, script) => fs.writeFileSync(path.join(directory, name),
    `#!/bin/sh\necho "$*" >> "$HOME/${name}.log"\n${script}\n`, { mode: 0o755 });
  const medium = (name, kind, extra) => `<dict><key>IORegistryEntryName</key><string>JetKVM Virtual Media Media</string>
    <key>IOObjectClass</key><string>${kind}</string><key>BSD Name</key><string>${name}</string>
    <key>IORegistryEntryID</key><integer>${name.slice(4)}</integer>${extra}</dict>`;
  const driver = (bytes, whole) => `<dict><key>Statistics</key><dict><key>Bytes (Read)</key><integer>${bytes}</integer></dict>
    <key>IORegistryEntryChildren</key><array>${whole}</array></dict>`;
  const size = bytes => `<key>Preferred Block Size</key><integer>${bytes}</integer>`;
  fs.writeFileSync(path.join(directory, 'registry'), `<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><array>
    ${driver('COUNT7', medium('disk7', 'IOCDMedia',
    `<key>IORegistryEntryChildren</key><array><dict><key>BSD Name</key><string>disk7s0</string>${size(2048)}</dict></array>`))}
    ${driver('COUNT8', medium('disk8', 'IOMedia', size(512)))}
    ${driver(9000000, medium('disk9', 'IOMedia', size(512)))}
    ${driver('COUNT5', medium('disk5', 'IOMedia', size(512)))}
    ${driver('COUNT6', medium('disk6', 'IOMedia', size(512)))}
    ${driver(1000, `<dict><key>IORegistryEntryName</key><string>APPLE SSD Media</string><key>BSD Name</key><string>disk0</string></dict>`)}
    </array></plist>`);
  fs.writeFileSync(path.join(directory, 'dd.log'), '');
  stub('ioreg', 'reads() { grep -c "if=/dev/r$1 " "$HOME/dd.log"; }\n' +
    'sed -e "s/COUNT7/$((5000000 + 2048 * $(reads disk7s0)))/" -e "s/COUNT8/$((7000000 + 512 * $(reads disk8)))/" ' +
    '-e "s/COUNT5/$((100000 * $(grep -c . "$HOME/ioreg.log") + 512 * $(reads disk5)))/" ' +
    '-e "s/COUNT6/$((6000000 + 512 * $(reads disk6)))/" "$HOME/registry"');
  stub('dd', 'case "$1" in if=/dev/rdisk9) exec sleep 4;; if=/dev/rdisk7s0) head -c 2048 /dev/zero;; ' +
    'if=/dev/rdisk6) sleep 1.2; head -c 512 /dev/zero;; *) head -c 512 /dev/zero;; esac');
  stub('diskutil', 'case "$2" in disk9) exec sleep 30;; esac');
  const noted = name => fs.readFileSync(path.join(directory, `${name}.log`), 'utf8').trim().split('\n');
  return { env: { PATH: `${directory}:/usr/bin:/bin`, HOME: directory }, noted };
}
// Runs a host command for at most `ms`; resolves with its output and time.
const runFor = (command, env, ms) => new Promise(resolve => {
  const start = Date.now();
  const child = spawn('sh', ['-c', command], { env, detached: true, stdio: ['ignore', 'pipe', 'inherit'] });
  let out = '';
  child.stdout.setEncoding('utf8').on('data', chunk => { out += chunk; });
  const timer = setTimeout(() => process.kill(-child.pid, 'SIGKILL'), ms);
  child.on('close', () => {
    clearTimeout(timer);
    resolve({ lines: out.trim().split('\n'), seconds: (Date.now() - start) / 1000 });
  });
});

test('the macOS watch counts only what a reader reads, keeps a medium whose read is slow, and does not wait at a disk without a medium', async () => {
  const mac = macWithMedia();
  const { lines } = await runFor(hostFor('Darwin').watchMedia, mac.env, 3400);
  const of = device => lines.filter(line => line.includes(`${device} `)).map(line => hostFor('Darwin').medium(line.split(' ')));
  const reads = device => mac.noted('dd').filter(line => line.includes(`if=${device} `)).length;
  // Nothing but the watch reads these two: it reads a block of each in
  // every round, and its count of them stays as it is.
  const rounds = of('/dev/rdisk8').length;
  assert.ok(rounds >= 4, `${rounds} rounds in 3.4 s`);
  assert.ok(reads('/dev/rdisk8') >= rounds && reads('/dev/rdisk7s0') >= rounds);
  const bytes = device => of(device).map(({ type, bytesRead }) => ({ type, bytesRead }));
  assert.deepEqual(bytes('/dev/rdisk7s0'), Array(rounds).fill({ type: 'rom', bytesRead: 5000000 }));
  assert.deepEqual(bytes('/dev/rdisk8'), Array(rounds).fill({ type: 'disk', bytesRead: 7000000 }));
  // The disk without a medium: one read, not one per round, and no line.
  assert.equal(reads('/dev/rdisk9'), 1);
  assert.deepEqual(of('/dev/rdisk9'), []);
  // The disk that a reader reads: the watch reads it in the first round
  // only, and its count is the reader's.
  assert.equal(reads('/dev/rdisk5'), 1);
  assert.deepEqual(of('/dev/rdisk5').map(medium => medium.bytesRead), of('/dev/rdisk5').map((_, round) => 100000 * (round + 1)));
  assert.equal(of('/dev/rdisk5').length, rounds);
  // The slow medium has a line from its first read on, in every round,
  // also in the rounds in which its next read is under way, and the block
  // of that read does not count.
  const first = lines.findIndex(line => line.includes('/dev/rdisk6 '));
  assert.ok(first > 0, lines.join('\n'));
  const after = lines.slice(first);
  const count = device => after.filter(line => line.includes(`${device} `)).length;
  assert.ok(count('/dev/rdisk6') >= 2 && count('/dev/rdisk6') >= count('/dev/rdisk8'), after.join('\n'));
  assert.deepEqual([...new Set(of('/dev/rdisk6').map(medium => medium.bytesRead))], [6000000]);
  // Each line ends with the reader's counts of the blocks that it checked.
  assert.deepEqual(lines.filter(line => !/^(rom|disk) \/dev\/rdisk[5-8](s0)? \d+ blocks=\d+,\d+,\d+$/.test(line)), []);
});

test('the macOS reader reads each medium again and again, with one read at a time of a disk without a medium', async () => {
  const mac = macWithMedia();
  // The reader is the watch's script in its mode "read".
  await runFor(hostFor('Darwin').watchMedia.replace(/ watch$/, ' read'), mac.env, 2600);
  const passes = device => mac.noted('dd').filter(line => line === `if=${device} bs=64k`).length;
  // A pass of these stand-ins ends at once, and the next starts a second
  // after its start.
  assert.ok(passes('/dev/rdisk8') >= 2 && passes('/dev/rdisk8') <= 4, `${passes('/dev/rdisk8')} passes`);
  assert.ok(passes('/dev/rdisk7s0') >= 2 && passes('/dev/rdisk7s0') <= 4, `${passes('/dev/rdisk7s0')} passes`);
  assert.equal(passes('/dev/rdisk9'), 1);
  // The reader makes no other read.
  const all = ['/dev/rdisk7s0', '/dev/rdisk8', '/dev/rdisk9', '/dev/rdisk5', '/dev/rdisk6'];
  assert.equal(mac.noted('dd').length, all.reduce((sum, device) => sum + passes(device), 0));
});

test('the macOS host ejects each disk of the Mini, and names the disk whose eject does not return in 5 s', async () => {
  const mac = macWithMedia();
  const { lines, seconds } = await runFor(hostFor('Darwin').releaseMedia, mac.env, 9000);
  assert.deepEqual(lines, ['disk9']);
  assert.deepEqual(mac.noted('diskutil'), ['eject disk8', 'eject disk9', 'eject disk5', 'eject disk6']);
  assert.ok(seconds >= 5 && seconds < 8, `took ${seconds} s`);
});

test('the bytes of a seed are the same in the page, differ from seed to seed, and do not repeat inside a block', () => {
  const inPage = (0, eval)(`(${String(seededBytes)})`);
  assert.deepEqual(inPage(7, 4096), seededBytes(7, 4096));
  assert.notEqual(sha256(seededBytes(7, 4096)), sha256(seededBytes(8, 4096)));
  // A part of a block at another place of it: 256 bytes moved by 256 bytes.
  const block = seededBytes(7, 4096);
  const moved = Uint8Array.from(block);
  moved.copyWithin(256, 0, 256);
  assert.notEqual(sha256(moved), sha256(block));
  // An image's block n is the block 0 of the image whose seed is n higher, and no two of its blocks are equal.
  const image = imageBytes(50, 3);
  assert.equal(image.length, 3 * IMAGE_BLOCK_BYTES);
  assert.deepEqual(blockChecksums(image).slice(1), blockChecksums(imageBytes(51, 2)));
  assert.equal(new Set(blockChecksums(image)).size, 3);
});

// Runs the host's command that says what a reader will read, and then the
// reader's check of `passes`, each the bytes of one pass over the medium;
// the host's files are in a directory of the test. Returns the reader's
// counts after each pass: checked, wrong at the first read, wrong later.
function checkedPasses(directory, expect, passes) {
  const local = command => command.replaceAll('/tmp/jetkvm-combined-load', path.join(directory, 'host'));
  const files = ['reader.blocks', 'blocks.sha256', 'reader.read'].map(name => path.join(directory, `host-${name}`));
  assert.equal(spawnSync('sh', ['-c', local(expect)]).status, 0);
  return passes.map(bytes => {
    const run = spawnSync('python3', ['-c', PYTHON_CHECK_INPUT, ...files], { input: bytes });
    assert.equal(run.status, bytes.length ? 0 : 1, String(run.stderr));
    return fs.readFileSync(files[0], 'utf8').split(' ').map(Number);
  });
}

test('the reader counts each whole block that it read, a wrong one apart at its first read, and no block of a pass that was cut off', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const image = imageBytes(300, 4);
  const sums = blockChecksums(image);
  const wrongAt = block => {
    const bytes = Buffer.from(image);
    bytes[block * IMAGE_BLOCK_BYTES + 5] ^= 1;
    return bytes;
  };
  const linux = hostFor('Linux');
  // The image on the TF card after its upload: block 2 is wrong at its first read.
  const first = linux.expectBlocks(sums, { firstReads: true, forget: true });
  assert.deepEqual(checkedPasses(directory, first, [wrongAt(2)]), [[4, 1, 0]]);
  // The next mount of it: the counts start at zero, and the same block, wrong again, is a later read of it.
  const again = linux.expectBlocks(sums, { firstReads: true });
  assert.deepEqual(checkedPasses(directory, again, [image, wrongAt(2)]), [[4, 0, 0], [8, 0, 1]]);
  // After a restart of the Mini the reader forgets what it read: block 0 is wrong at its first read again,
  // and in the pass after that at a later one.
  assert.deepEqual(checkedPasses(directory, first, [wrongAt(0), wrongAt(0)]), [[4, 1, 0], [8, 1, 1]]);
  // An image from a URL has no first reads.
  assert.deepEqual(checkedPasses(directory, linux.expectBlocks(sums), [wrongAt(3)]), [[4, 0, 1]]);
  // A pass that ended inside block 2, and a medium that gave no byte, which fails the reader's command.
  assert.deepEqual(checkedPasses(directory, again, [image.subarray(0, 2 * IMAGE_BLOCK_BYTES + 100), Buffer.alloc(0)]),
    [[2, 0, 0], [2, 0, 0]]);
  // A medium that is longer than the image that the tool named: the blocks beyond are not counted.
  assert.deepEqual(checkedPasses(directory, linux.expectBlocks(sums.slice(0, 2)), [image]), [[2, 0, 0]]);
});

test('every host\'s watch line gives the reader\'s counts of blocks, and a line without them gives none', () => {
  const lines = { Linux: 'disk /dev/sda 407 0 16384 52 0 0 0 0 0 96 52', Darwin: 'disk /dev/rdisk4 8388608',
    'MINGW64_NT-10.0': 'disk //./PhysicalDrive2 00000000000002883584' };
  for (const [system, line] of Object.entries(lines)) {
    const counted = hostFor(system).medium(`${line} blocks=64,1,2`.split(' '));
    assert.deepEqual([counted.blocksChecked, counted.blocksWrongFirst, counted.blocksWrongLater], [64, 1, 2], system);
    // The Linux watch prints the field empty while the reader has written no counts.
    for (const without of [line, `${line} blocks=`]) assert.equal(hostFor(system).medium(without.split(' ')).blocksChecked, undefined, system);
  }
});

// A recording with a mount of the TF card from second 2 on, in which the
// host's reader has these counts in each second: [wrong at the first read,
// wrong later]; a missing value is the one before.
function readBlocks(seconds, counts, state = second => ({ video: true, tfMountDisk: second >= 2 })) {
  const rows = recording(seconds, state);
  let now = [0, 0];
  rows.forEach((row, second) => {
    now = counts[second] ?? now;
    const name = Object.keys(row.state).find(variable => /Mount/.test(variable) && row.state[variable]);
    row.loads = name ? { [name]: { bytesRead: second * 1_000_000, blocksChecked: second * 7, blocksWrongFirst: now[0], blocksWrongLater: now[1] } } : {};
  });
  return rows;
}

test('uploadsCorrupt and mountReadsCorrupt are the growth of the reader\'s two counts, from zero at each mount', () => {
  // A wrong block before the mount's first sample is in that sample.
  const rows = readBlocks(10, { 2: [1, 0], 5: [1, 2], 7: [3, 2] });
  assert.deepEqual(values(rows, 'uploadsCorrupt'), [[2000, 1], [7000, 2]]);
  assert.deepEqual(values(rows, 'mountReadsCorrupt'), [[5000, 2]]);
  // A second mount counts from zero again: its count of 1 is one more wrong block, not the first mount's.
  const twice = readBlocks(12, { 3: [0, 1], 8: [0, 1] }, second => ({ video: true, tfMountDisk: second >= 2 && second < 6, httpMountCdrom: second >= 8 }));
  assert.deepEqual(values(twice, 'mountReadsCorrupt'), [[3000, 1], [8000, 1]]);
  // A host whose watch gives no counts, as with a reader before them, has no value.
  const without = readBlocks(6, {});
  without.forEach(row => { for (const load of Object.values(row.loads)) { delete load.blocksWrongFirst; delete load.blocksWrongLater; } });
  assert.equal(judge(without, rules({ measure: ['uploadsCorrupt', 'mountReadsCorrupt'], max: 0 })).result, 'pass');
});

test('fileTransfersCorrupt is the growth of its count, clipboardCorrupt counts the rounds with another text, and a transfer or a round that only failed is in neither', () => {
  const rows = recording(8, { video: true, osFiles: true, osClipboard: true });
  rows.forEach((row, second) => {
    row.loads = {
      osFiles: { bytesSent: second * 400_000, broken: Number(second >= 2), failed: Number(second >= 4), corrupt: Number(second >= 4) },
      osClipboard: clipboardLoad(row, second === 3 ? null : second === 6 ? false : true),
    };
  });
  assert.deepEqual(values(rows, 'fileTransfersCorrupt'), [[4000, 1]]);
  assert.deepEqual(values(rows, 'filesFailed'), [[2000, 1], [4000, 1]]);
  assert.deepEqual(values(rows, 'clipboardCorrupt'), [[6000, 1]]);
  assert.deepEqual(values(rows, 'clipboardWrong'), [[3000, 1], [6000, 1]]);
});

test('on both boards a wrong block, file or clipboard text fails in its second in every state of the walk, and nothing fails without one', () => {
  const walk = readJsonc(path.join(here, '..', 'lists.jsonc')).walk;
  // The host reads the image on the TF card again after USB emulation was off and on.
  const again = walk.findIndex(step => step.state.usbEmulation);
  assert.deepEqual(walk[again].state, { video: true, mqtt: true, cloud: true, osServices: true, hid: true, tfMountCdrom: true, usbEmulation: true });
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const failed = rows => judge(rows, criteria).failures.filter(f => /Corrupt$/.test(f.measure)).map(f => [f.measure, f.t - T0, f.value]);
    const mounted = () => walk[again].state;
    assert.deepEqual(failed(readBlocks(8, {}, mounted)), [], board);
    assert.deepEqual(failed(readBlocks(8, { 4: [1, 0] }, mounted)), [['uploadsCorrupt', 4000, 1]], board);
    assert.deepEqual(failed(readBlocks(8, { 4: [0, 1] }, mounted)), [['mountReadsCorrupt', 4000, 1]], board);
    const session = recording(8, { video: true, osServices: true, osFiles: true, osClipboard: true });
    session.forEach((row, second) => {
      row.loads = { osFiles: { bytesSent: second * 400_000, broken: 0, failed: Number(second >= 3), corrupt: Number(second >= 3) },
        osClipboard: clipboardLoad(row, second !== 5) };
    });
    assert.deepEqual(failed(session), [['fileTransfersCorrupt', 3000, 1], ['clipboardCorrupt', 5000, 1]], board);
  }
});

test('the page sends file n with the bytes of the seed n, and the tool counts a file with another checksum as corrupt and one without a checksum as failed only', async () => {
  // The page's side: a peer whose channel notes what is sent, and a Mini that gives credit for the whole file.
  const size = 100_000;
  const sent = [];
  const channel = { readyState: 'open', bufferedAmount: 0, send: data => sent.push(data), close() { this.onclose(); } };
  const peer = { connectionState: 'connected', createDataChannel: () => channel };
  const hooks = { _getPeerConnection: () => peer,
    sendJsonRpc: (method, params, answer) => answer({ result: { channel: 'guestfile_1', transfers: [9] } }) };
  globalThis.window = { __kvmTestHooks: hooks };
  try {
    startFiles([size, String(seededBytes)]);
    const files = globalThis.window.__combinedLoadFiles;
    channel.onopen();
    channel.onmessage({ data: JSON.stringify({ type: 'credit', bytes: size }) });
    await until(() => files.bytesSent === size, 1000, 'the file was not sent');
    files.wanted = false;
    assert.deepEqual(Buffer.concat(sent.map(message => Buffer.from(message.subarray(4)))), Buffer.from(seededBytes(1, size)));
  } finally {
    delete globalThis.window;
  }
  // The tool's side: three finished transfers of 1 MiB files. The host has file 1 as it was sent, file 2 with
  // the bytes of file 1, and no file 3. The host is this machine, with the commands of a Linux host.
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const at = file => path.join(directory, `combined-load-${file}.bin`);
  fs.writeFileSync(at(1), seededBytes(1, IMAGE_BLOCK_BYTES));
  fs.writeFileSync(at(2), seededBytes(1, IMAGE_BLOCK_BYTES));
  const done = [1, 2, 3].map(file => ({ file, status: 'ok', path: at(file) }));
  const page = { evaluate: async script => (String(script).includes('done.splice') ? done.splice(0)
    : String(script).includes('bytesSent, broken') ? { bytesSent: 3 * IMAGE_BLOCK_BYTES, broken: 0 } : undefined) };
  const commands = [];
  const hostExec = async command => {
    commands.push(command);
    return spawnSync('sh', ['-c', command], { encoding: 'utf8' }).stdout;
  };
  const { osFiles } = osServicesParts({ page, call: null, hostExec, host: hostFor('Linux') });
  await osFiles.on();
  await osFiles.off();
  const { verified, failed, corrupt } = await osFiles.sample();
  assert.deepEqual({ verified, failed, corrupt }, { verified: 1, failed: 2, corrupt: 1 });
  assert.deepEqual(fs.readdirSync(directory), []);
  // The three files of that round were checked with one command.
  assert.equal(commands.length, 1);
  // A Windows host's line for a path with backslashes begins with one, and has them twice.
  const sum = sha256(seededBytes(7, IMAGE_BLOCK_BYTES));
  let asked = 0;
  const windows = osServicesParts({ page: { evaluate: async script => (String(script).includes('done.splice') ? asked++ ? []
    : [{ file: 7, status: 'ok', path: 'C:\\Users\\runner\\Downloads\\combined-load-7.bin' }, { file: 17, status: 'ok', path: 'C:\\Users\\runner\\Downloads\\combined-load-17.bin' }]
    : String(script).includes('bytesSent, broken') ? { bytesSent: 0, broken: 0 } : undefined) },
  call: null, hostExec: async () => `\\${sum} *C:\\\\Users\\\\runner\\\\Downloads\\\\combined-load-7.bin\n`, host: hostFor('MINGW64_NT-10.0') }).osFiles;
  await windows.on();
  await windows.off();
  const checked = await windows.sample();
  assert.deepEqual({ verified: checked?.verified, failed: checked?.failed }, { verified: 1, failed: 1 });
});

test('osFilesSmall drops 4 KiB files, the next one when the one before has its outcome, works from the first file on, and its stop leaves the other file transfer', async () => {
  const channels = [];
  const peer = { connectionState: 'connected', createDataChannel: label => {
    const channel = { label, readyState: 'open', bufferedAmount: 0, sent: 0, send(data) { this.sent += data.byteLength - 4; }, close() { this.onclose(); } };
    channels.push(channel);
    return channel;
  } };
  let drops = 0;
  const sizes = [];
  const hooks = { _getPeerConnection: () => peer, sendJsonRpc: (method, params, answer) => {
    sizes.push(params.drop.files[0].size);
    answer({ result: { channel: `guestfile_${++drops}`, transfers: [drops] } });
  } };
  globalThis.window = { __kvmTestHooks: hooks };
  const page = { evaluate: async (script, argument) => script(argument) };
  const { osFiles, osFilesSmall } = osServicesParts({ page, call: null, hostExec: async () => '', host: { checksumsAndRemove: files => files.join(' ') } });
  // The Mini gives a transfer credit for its file and reports it as done.
  const finish = async channel => {
    channel.onopen();
    channel.onmessage({ data: JSON.stringify({ type: 'credit', bytes: 1024 * 1024 }) });
    await until(() => channel.sent === 4096, 1000, 'the file was not sent');
    channel.onmessage({ data: JSON.stringify({ type: 'done', status: 'ok', path: '/h/file' }) });
  };
  try {
    await osFilesSmall.on();
    assert.equal(await osFilesSmall.works(true), false);
    await finish(channels[0]);
    assert.equal(await osFilesSmall.works(true), true);
    // The second drop began without a wait, on a channel of its own.
    await until(() => channels.length === 2, 500, 'no second drop within half a second of the first one\'s outcome');
    await finish(channels[1]);
    await until(() => channels.length === 3, 500, 'no third drop');
    assert.deepEqual(sizes, [4096, 4096, 4096]);
    assert.deepEqual(channels.map(channel => channel.label), ['guestfile_1', 'guestfile_2', 'guestfile_3']);
    const { started, bytesSent, broken } = await osFilesSmall.sample();
    assert.deepEqual({ started, bytesSent, broken }, { started: 3, bytesSent: 8192, broken: 0 });
    // The other file transfer's stop does not stop this one.
    await osFiles.off();
    assert.equal(globalThis.window.__combinedLoadFiles.wanted, true);
    await osFilesSmall.off();
    assert.equal(globalThis.window.__combinedLoadFiles.wanted, false);
  } finally {
    // Also after a failed check: the switch's timer ends, and the page sends no further file.
    await osFilesSmall.off();
    delete globalThis.window;
  }
});

test('a clipboard round with another text back is corrupt and wrong, and a round without an answer is wrong only', async () => {
  const answers = [text => ({ text }), () => ({ text: 'another text' }), () => { throw new Error('timeout'); }];
  let set = null;
  let round = 0;
  const call = async (method, params) => {
    if (method === 'guestClipboardSet') return void (set = params.text);
    return answers[round++](set);
  };
  const { osClipboard } = osServicesParts({ page: null, call, hostExec: null, host: {} });
  await osClipboard.on();
  // Each sample has the rounds that ended since the one before.
  const ended = [];
  const sample = () => {
    const taken = osClipboard.sample();
    ended.push(...taken.ended);
    return taken;
  };
  try {
    await until(() => sample().rounds === 3, 5000, 'no three clipboard rounds in 5 s');
  } finally {
    await osClipboard.off();
  }
  const { wrong, corrupt } = sample();
  assert.deepEqual({ wrong, corrupt }, { wrong: 2, corrupt: 1 });
  // Each round once, with its start, its time and what came back.
  assert.deepEqual(ended.map(([startedAt, ms, back]) => [typeof startedAt, ms >= 0, back]),
    [['number', true, true], ['number', true, false], ['number', true, null]]);
});

test('clipboardMs is the slowest round of the sample in which the host answered, a round under way counts with its time so far, and a round without an answer does not count', () => {
  const rows = recording(7, { video: true, osClipboard: true });
  const rounds = [
    { ended: [[0, 80, true], [1000, 95, true]], runningSince: null },
    // A round without an answer after 3 s: no time of its own.
    { ended: [[0, 3000, null]], runningSince: null },
    // A slow round under way for 1.2 s at the sample, then ended after 1.9 s.
    { ended: [], runningSince: -1200 },
    { ended: [[-1900, 1900, true]], runningSince: null },
    // A round with another text back has its time.
    { ended: [[0, 70, false]], runningSince: null },
    { ended: [], runningSince: null },
  ];
  rows.forEach((row, second) => {
    if (second > 0) row.loads = { osClipboard: { rounds: second, wrong: 0, corrupt: 0, at: 0, ...rounds[second - 1] } };
  });
  assert.deepEqual(valueEachSecond(rows, 'clipboardMs'), [null, 95, null, 1200, 1900, 70, null]);
});

test('the terminal answers each cursor question once, also one in two messages, and counts a number beside a control sequence', () => {
  // The page's side of startTerminal: a peer connection whose channel notes what is sent.
  const sent = [];
  const channel = { readyState: 'open', send: data => sent.push(new TextDecoder().decode(data)), close() {} };
  const peer = { connectionState: 'connected', createDataChannel: () => channel };
  globalThis.window = { __kvmTestHooks: { _getPeerConnection: () => peer } };
  try {
    startTerminal('count\r');
    for (const text of ['\x1b[6n', '\x1b[', '6n1\r\n', '2\x1b[?25h\r\n3\r\n']) {
      channel.onmessage({ data: new TextEncoder().encode(text).buffer });
    }
    assert.deepEqual(sent, ['\x1b[1;1R', '\x1b[1;1R']);
    const { lines, breaks } = globalThis.window.__combinedLoadTerminal;
    assert.deepEqual({ lines, breaks }, { lines: 3, breaks: [] });
  } finally {
    delete globalThis.window;
  }
});

test('the light terminal types a line every period once the host is ready, times each answer on the page\'s clock, and gives up on a line after its time', () => {
  const sent = [];
  const channel = { readyState: 'open', send: data => sent.push(new TextDecoder().decode(data)), close() {} };
  const peer = { connectionState: 'connected', createDataChannel: () => channel };
  globalThis.window = { __kvmTestHooks: { _getPeerConnection: () => peer } };
  const say = text => channel.onmessage({ data: new TextEncoder().encode(text).buffer });
  // The page's timer and clock are the test's.
  const real = { setInterval: globalThis.setInterval, setTimeout: globalThis.setTimeout, now: Date.now };
  let type = null;
  let clock = 50_000;
  globalThis.setInterval = callback => { type = callback; return 0; };
  Date.now = () => clock;
  try {
    startTerminal(['answer\r', 'osTerminalLight', 100]);
    // Nothing is typed before the host's "ready"; the echo of the typed command is not it.
    type();
    say('$ perl -pe \'BEGIN{$|=1;print "ready\\n"}s/^e/r/\'\r\n');
    type();
    assert.deepEqual(sent, []);
    // zsh: the end of the bracketed paste, a carriage return and the window's title before the command's output.
    say('\x1b[?1l\x1b>\x1b[?2004l\r\r\n\x1b]2;perl -pe\x07\x1b]1;perl\x07ready\r\n');
    for (const at of [50_000, 50_100, 50_200]) {
      clock = at;
      type();
    }
    assert.deepEqual(sent, ['e1\r', 'e2\r', 'e3\r']);
    // The echo of a line is not its answer; an answer to no line is not taken.
    clock = 50_230;
    say('e1\r\nr1\r\ne2\r\n');
    clock = 50_400;
    say('r2\r\nr99\r\n');
    assert.deepEqual(takeRoundTrips(3000), { bytes: globalThis.window.__combinedLoadTerminal.bytes, sent: 3, answered: 2,
      roundTrips: [[50_000, 230], [50_100, 300]] });
    // Line 3 has no answer 3 s after its send, and its answer after that is not taken.
    clock = 53_199;
    assert.deepEqual(takeRoundTrips(3000).roundTrips, []);
    clock = 53_200;
    assert.deepEqual(takeRoundTrips(3000).roundTrips, [[50_200, null]]);
    say('r3\r\n');
    assert.deepEqual([takeRoundTrips(3000).roundTrips, globalThis.window.__combinedLoadTerminal.answered], [[], 2]);
    // A new channel, after the session ended, waits for the host's "ready" again.
    globalThis.setTimeout = callback => callback();
    channel.onclose();
    type();
    assert.equal(sent.length, 3);
    say('ready\r\n');
    type();
    assert.deepEqual(sent.slice(3), ['e4\r']);
    // The heavy terminal types nothing and takes no round trips.
    startTerminal('count\r');
    assert.equal(takeRoundTrips(3000), null);
  } finally {
    Object.assign(globalThis, { setInterval: real.setInterval, setTimeout: real.setTimeout });
    Date.now = real.now;
    delete globalThis.window;
  }
});

test('the light terminal\'s answer of a Linux or macOS host says ready and answers each line at once, and each of the tool\'s terminals stops only itself', async () => {
  for (const system of ['Linux', 'Darwin']) {
    const typed = hostFor(system).answerInTerminal;
    assert.match(typed, /\r$/, system);
    // A pipe in place of the terminal: each answer comes before the next line is written.
    const child = spawn('sh', ['-c', typed.slice(0, -1)], { stdio: ['pipe', 'pipe', 'inherit'] });
    let out = '';
    child.stdout.setEncoding('utf8').on('data', chunk => { out += chunk; });
    try {
      for (const [line, answer] of [[null, 'ready\n'], ['e1', 'r1\n'], ['e22', 'r22\n']]) {
        if (line) child.stdin.write(`${line}\n`);
        await until(() => out.endsWith(answer), 3000, `${system}: no "${answer.trim()}" after ${JSON.stringify(out)}`);
      }
      assert.equal(out, 'ready\nr1\nr22\n', system);
    } finally {
      child.kill();
    }
  }
  assert.match(hostFor('MINGW64_NT-10.0').answerInTerminal, /^\[Console\]::Out\.Write\("ready`r`n"\);.*\[Console\]::In\.ReadLine\(\).*-replace '\^e','r'/);
  // The page has one terminal of the tool: the switch of the other kind does not report it as its own and does not stop it.
  const channel = { readyState: 'open', send() {}, close() { this.closed = true; } };
  const peer = { connectionState: 'connected', createDataChannel: () => channel };
  globalThis.window = { __kvmTestHooks: { _getPeerConnection: () => peer } };
  const page = { evaluate: async (script, argument) => script(argument) };
  const { osTerminal, osTerminalLight } = osServicesParts({ page, call: null, hostExec: null,
    host: { countInTerminal: 'flood\r', answerInTerminal: 'light\r' } });
  try {
    await osTerminalLight.on();
    assert.deepEqual([await osTerminalLight.isOn(), await osTerminal.isOn()], [true, false]);
    await osTerminal.off();
    assert.deepEqual([await osTerminalLight.isOn(), channel.closed], [true, undefined]);
    await osTerminalLight.off();
    assert.deepEqual([await osTerminalLight.isOn(), channel.closed], [false, true]);
  } finally {
    globalThis.window.__combinedLoadTerminal.wanted = false;
    clearInterval(globalThis.window.__combinedLoadTerminal.typer);
    delete globalThis.window;
  }
});

test('on both boards the light terminal\'s round trips have a limit and the flood has none, and the walk has the light terminal beside the session\'s other loads', () => {
  const walk = readJsonc(path.join(here, '..', 'lists.jsonc')).walk.map(step => Object.keys(step.state).filter(name => step.state[name]).sort());
  const light = walk.filter(names => names.includes('osTerminalLight'));
  assert.deepEqual(light, [['hid', 'httpMountDisk', 'osServices', 'osTerminalLight', 'tailscale', 'video'],
    ['cloud', 'hid', 'mqtt', 'osClipboard', 'osFiles', 'osServices', 'osTerminalLight', 'video'],
    ['cloud', 'hid', 'httpMountDisk', 'mqtt', 'osServices', 'osTerminalLight', 'osVideo', 'video'],
    ['cloud', 'httpsMountDisk', 'mqtt', 'osServices', 'osTerminalLight', 'video'],
    ['cloud', 'httpsMountDisk', 'mqtt', 'osServices', 'osTerminalLight', 'ota', 'video']]);
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    // Ten round trips per second of 40 ms, and in one second some slower ones.
    const rows = (variable, slow) => recording(16, { video: true, osServices: true, [variable]: true }).map((row, second) => ({ ...row,
      loads: { [variable]: { bytes: second * 400_000, lines: second * 1000, breaks: [],
        roundTrips: Array.from({ length: 10 }, (_, i) => [row.t - 1000 + i * 100, i < (slow[second]?.length ?? 0) ? slow[second][i] : 40]) } } }));
    const late = (variable, slow) => judge(rows(variable, slow), criteria).failures
      .filter(f => f.measure.startsWith('terminalLine')).map(f => [f.measure, f.t - T0, f.value]);
    // Two slow lines of 50 in the window are inside its 95th percentile; three are not.
    assert.deepEqual(late('osTerminalLight', { 8: [450, 450] }), [], board);
    assert.deepEqual(late('osTerminalLight', { 8: [450, 450, 450] }), [['terminalLineP95Ms', 8000, 450]], board);
    // One line over 500 ms, or without an answer.
    assert.deepEqual(late('osTerminalLight', { 8: [600] }), [['terminalLineMaxMs', 8000, 600]], board);
    assert.deepEqual(late('osTerminalLight', { 8: [null] }), [['terminalLineMaxMs', 8000, 3000]], board);
    assert.deepEqual(late('osTerminal', { 8: [600, 600, 600] }), [], board);
  }
});

test('the printer of a Linux or macOS host begins each write of about 8 KiB with its clock, and its numbers have no gap', () => {
  for (const system of ['Linux', 'Darwin']) {
    const command = hostFor(system).countInTerminal;
    assert.match(command, /\r$/, system);
    const before = Date.now();
    const out = spawnSync('sh', ['-c', `${command.slice(0, -1)} | head -c 100000`], { encoding: 'utf8' }).stdout;
    const lines = out.split('\n').slice(0, -1);
    const stamps = lines.flatMap((line, index) => (line.startsWith('t') ? [index] : []));
    // 100000 bytes are 12 writes; each begins with a stamp of now.
    assert.equal(stamps[0], 0, system);
    assert.ok(stamps.length >= 12, `${system}: ${stamps.length} stamps`);
    for (const index of stamps) {
      assert.match(lines[index], /^t\d{13}$/, system);
      assert.ok(Math.abs(Number(lines[index].slice(1)) - before) < 5000, `${system}: ${lines[index]}`);
    }
    const bytesOf = (from, to) => lines.slice(from, to).join('\n').length + 1;
    for (let n = 1; n < stamps.length; n++) {
      const bytes = bytesOf(stamps[n - 1], stamps[n]);
      assert.ok(bytes >= 8192 && bytes < 8192 + 16, `${system}: a write of ${bytes} bytes`);
    }
    const numbers = lines.filter(line => !line.startsWith('t')).map(Number);
    assert.deepEqual(numbers.slice(0, 2000), Array.from({ length: 2000 }, (_, i) => i + 1), system);
    assert.ok(numbers.every((number, i) => number === i + 1), system);
  }
  // PowerShell on Windows: the same line before each write of 1000 numbers.
  assert.match(hostFor('MINGW64_NT-10.0').countInTerminal, /Out\.Write\("t"\+\[DateTimeOffset\]::UtcNow\.ToUnixTimeMilliseconds\(\)\+"`r`n"\+/);
});

test('a file transfer whose channel closes before its outcome is counted as closed while the connection is up, and a drop that the Mini does not take as refused', async () => {
  const channels = [];
  const peer = { connectionState: 'connected', createDataChannel: () => {
    const channel = { readyState: 'open', bufferedAmount: 0, send() {}, close() { this.onclose(); } };
    channels.push(channel);
    return channel;
  } };
  // The Mini takes the first drop, refuses the second ("a file transfer is already in progress") and takes the third.
  const answers = [{ result: { channel: 'guestfile_1', transfers: [1] } }, { error: { message: 'a file transfer is already in progress' } },
    { result: { channel: 'guestfile_2', transfers: [2] } }];
  let drops = 0;
  const hooks = { _getPeerConnection: () => peer, sendJsonRpc: (method, params, answer) => answer(answers[Math.min(drops++, 2)]) };
  globalThis.window = { __kvmTestHooks: hooks };
  try {
    startFiles([1000, String(seededBytes)]);
    const files = globalThis.window.__combinedLoadFiles;
    // The Mini closes the first transfer's channel without credit and without an outcome.
    channels[0].onopen();
    channels[0].onclose();
    assert.deepEqual({ broken: files.broken, closed: files.closed, refused: files.refused }, { broken: 1, closed: 1, refused: 0 });
    // The page sends the file again a second later; that drop is refused, and the one after it is taken.
    await until(() => channels.length === 2, 4000, 'no drop after the refused one');
    assert.deepEqual({ broken: files.broken, refused: files.refused }, { broken: 1, refused: 1 });
    // A transfer with its outcome is not a closed one.
    channels[1].onopen();
    channels[1].onmessage({ data: JSON.stringify({ type: 'done', status: 'ok', path: '/h/1' }) });
    assert.deepEqual({ broken: files.broken, closed: files.closed, done: files.done.length }, { broken: 1, closed: 1, done: 1 });
    // The next transfer ends because the connection went away, as at a restart of the Mini: broken, not closed by the Mini.
    await until(() => channels.length === 3, 4000, 'no drop after the finished one');
    channels[2].onopen();
    peer.connectionState = 'disconnected';
    channels[2].onclose();
    files.wanted = false;
    assert.deepEqual({ broken: files.broken, closed: files.closed }, { broken: 2, closed: 1 });
  } finally {
    delete globalThis.window;
  }
});

test('a transfer that ends when the Mini restarts is neither failed nor closed by the Mini, and one that ends 10 s before the restart is both', () => {
  // The Mini's clock starts again at 20.5 s: its answer of second 22 has 1500 ms.
  const rows = recording(30, { video: true, osFiles: true });
  const closedAt = closes => rows.map((row, second) => ({ ...row,
    device: { nowMs: second >= 22 ? (second - 22) * 1000 + 1500 : 60_000 + second * 1000 },
    loads: { osFiles: { bytesSent: second * 300_000, failed: 0,
      broken: closes.filter(at => second >= at).length, closed: closes.filter(at => second >= at).length } } }));
  const measured = (closes, measure) => values(closedAt(closes), measure);
  for (const measure of ['filesFailed', 'fileTransfersClosed']) {
    assert.deepEqual(measured([19], measure), [], measure);
    assert.deepEqual(measured([10], measure), [[10_000, 1]], measure);
    // The count of the restart's transfer is not carried into the seconds after it.
    assert.deepEqual(measured([19, 26], measure), [[26_000, 1]], measure);
  }
  // A transfer that the Mini reported as failed in the seconds of the restart is a failed one.
  const failedAt19 = closedAt([]).map((row, second) => ({ ...row, loads: { osFiles: { ...row.loads.osFiles, failed: Number(second >= 19) } } }));
  assert.deepEqual(values(failedAt19, 'filesFailed'), [[19_000, 1]]);
});

test('the walk ends service video and turns it on again while small files are dropped and the clipboard goes round, and both boards fail a drop or a round that fails then', () => {
  const walk = readJsonc(path.join(here, '..', 'lists.jsonc')).walk;
  const dropping = walk.map((step, index) => ({ ...step.state, index })).filter(state => state.osFilesSmall);
  // Three states in a row, with service video on, off and on: the driver changes only osVideo between them.
  assert.deepEqual(dropping.map(state => Boolean(state.osVideo)), [true, false, true]);
  assert.deepEqual(dropping.map(state => state.index - dropping[0].index), [0, 1, 2]);
  for (const state of dropping) assert.ok(state.osClipboard && state.osServices && !state.osFiles);
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const { index, ...state } = dropping[1];
    const rows = [change('osVideo', false, 2, 5, { state }), ...recording(10, state)];
    rows.filter(row => row.device).forEach((row, second) => {
      row.loads = { osFilesSmall: { bytesSent: second * 20_000, started: second * 5, broken: Number(second >= 4), closed: Number(second >= 4), failed: 0, corrupt: 0 },
        osClipboard: clipboardLoad(row, second === 5 ? null : true) };
    });
    assert.deepEqual(judge(rows, criteria).failures.map(f => [f.measure, f.t - T0, f.value]),
      [['filesFailed', 4000, 1], ['clipboardWrong', 5000, 1]], board);
  }
});

test('sessionGoneSeconds counts the seconds in a row in which the Mini answers without a session, not those of a restart, and more than 4 fail during an update on both boards', () => {
  const state = { video: true, ota: true };
  // gone: the seconds without a connected session. The Mini's clock starts again at restartAt.
  const rows = (gone, { restartAt = null, noDevice = [] } = {}) => recording(40, state, { restartAt, noDevice })
    .map((row, second) => ({ ...row, browser: gone.includes(second) ? { connectionState: 'disconnected', video: null } : row.browser }));
  const limit = rules({ while: { ota: true }, measure: 'sessionGoneSeconds', max: 4 });
  const failed = (list, criteria = limit) => judge(list, criteria).failures.filter(f => f.measure === 'sessionGoneSeconds').map(f => [f.t - T0, f.value]);
  assert.deepEqual(failed(rows(range(10, 13))), []);
  assert.deepEqual(failed(rows(range(10, 14))), [[14_000, 5]]);
  // A page without a peer connection has no session either.
  assert.deepEqual(failed(rows([]).map((row, second) => ({ ...row, browser: second >= 10 && second <= 14 ? null : row.browser }))), [[14_000, 5]]);
  // A second without the Mini's answer is not counted and does not end the count.
  assert.deepEqual(failed(rows(range(10, 15), { noDevice: [12] })), [[15_000, 5]]);
  assert.deepEqual(failed(rows(range(10, 14), { noDevice: [12] })), []);
  // The restart: the session ends at second 17, the Mini answers until 19 and again from 25 on, and the page's new session is there at 34.
  assert.deepEqual(failed(rows(range(17, 33), { restartAt: 22, noDevice: range(20, 24) })), []);
  // A session that died 8 s before the Mini stopped answering for its restart fails, and so does one that died 5 s before.
  assert.deepEqual(failed(rows(range(12, 33), { restartAt: 22, noDevice: range(20, 24) })), [[16_000, 5]]);
  assert.deepEqual(failed(rows(range(15, 33), { restartAt: 22, noDevice: range(20, 24) })), [[19_000, 5]]);
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    assert.deepEqual(failed(rows(range(10, 14)), criteria), [[14_000, 5]], board);
    assert.deepEqual(criteria.changeTakesAtMost.find(line => line.change.ota === true).pauses.includes('sessionGoneSeconds'), false, board);
    assert.equal(criteria.changeTakesAtMost.find(line => line.change.mqtt === true).max, 2, board);
  }
});

test('the light terminal\'s lines without an answer in the seconds of a restart of the Mini are not a slow session, and a line without one before the restart is', () => {
  // The Mini's clock starts again at 20.5 s: its answer of second 22 has 1500 ms. The page's lines get no answer from second 17 on,
  // the browser sees its session end at second 21, and the page, which loads again at second 25, has the old terminal until then.
  const rows = recording(30, { video: true, osTerminalLight: true });
  const trips = (second, late) => (second >= 17 ? [[0, null]] : [[0, late[second] ?? 12]]);
  const during = late => rows.map((row, second) => ({ ...row,
    device: { nowMs: second >= 22 ? (second - 22) * 1000 + 1500 : 60_000 + second * 1000 },
    browser: second >= 21 && second <= 24 ? { connectionState: 'disconnected', video: null } : row.browser,
    loads: { osTerminalLight: second > 24 ? null : { roundTrips: trips(second, late) } } }));
  const limit = rules({ while: { osTerminalLight: true }, measure: 'terminalLineMaxMs', max: 500 });
  assert.deepEqual(judge(during({}), limit).failures.map(f => [f.t - T0, f.value]), []);
  assert.deepEqual(judge(during({ 8: 2900, 9: null, 10: 15 }), limit).failures.map(f => [f.t - T0, f.value]), [[8000, 2900]]);
  // Without a restart, lines without an answer fail from the first on.
  const noRestart = recording(30, { video: true, osTerminalLight: true })
    .map((row, second) => ({ ...row, loads: { osTerminalLight: { roundTrips: trips(second, {}) } } }));
  assert.deepEqual(judge(noRestart, limit).failures.map(f => [f.t - T0, f.value]), [[17_000, 3000]]);
});

test('the light terminal\'s lines of a second without the browser\'s report are judged when the reports around it are of one connected session', () => {
  // The page's report does not come in seconds 8 and 9, and the line of second 9 has no answer.
  const rows = peerAfter => recording(16, { video: true, osTerminalLight: true }).map((row, second) => ({ ...row,
    browser: second === 8 || second === 9 ? null : { ...row.browser, peer: second > 9 ? peerAfter : '1.1' },
    loads: { osTerminalLight: { roundTrips: [[row.t - 500, second === 9 ? null : 12]] } } }));
  const limit = rules({ while: { osTerminalLight: true }, measure: 'terminalLineMaxMs', max: 500 });
  assert.deepEqual(judge(rows('1.1'), limit).failures.map(f => [f.t - T0, f.value]), [[9000, 3000]]);
  // The report after them is of a new peer connection: the session ended in between, and its lines are not judged.
  assert.deepEqual(judge(rows('1.2'), limit).failures.map(f => [f.t - T0, f.value]), []);
});

test('the measures of the video stream compare two reports of one stream in one session of the page, and a new session is not a replaced stream', () => {
  // 30 frames and 2 keyframes per second; the page's second session begins at second 10, with counts from zero.
  const rows = recording(20, { video: true }).map((row, second) => {
    const since = second >= 10 ? second - 10 : second;
    return { ...row, device: { ...row.device, accessUnitsSent: second * 30 },
      browser: { connectionState: 'connected', peer: second >= 10 ? 2 : 1, video: { id: 'IT01V1', ssrc: 1, timestamp: T0 + second * 1000,
        framesDecoded: since * 30, keyFramesDecoded: since * 2, framesDropped: 0, pliCount: 0 } } };
  });
  const measured = (list, measure, limit) => judge(list, rules({ measure, ...limit })).failures.map(f => [f.t - T0, Number(f.value.toFixed(2))]);
  const changed = change => rows.map((row, second) => ({ ...row, browser: { ...row.browser, video: { ...row.browser.video, ...change(second) } } }));
  // Nothing fails, also not at the new session, whose counts are lower than the old one's.
  for (const [measure, limit] of [['keyframesPerSecond', { min: 1.9 }], ['browserFramesDropped', { max: 0 }], ['keyframeRequests', { max: 0 }],
    ['videoStreamReplaced', { max: 0 }], ['framesDecodedShare', { min: 0.95 }]]) assert.deepEqual(measured(rows, measure, limit), [], measure);
  // A second without a keyframe, three dropped frames, a request for a keyframe.
  // The keyframes of the last 3 s: one second without its two is 4 in 3 s, and the stream that loses every second keyframe from
  // second 4 on has 5 in the 3 s up to second 4 and 3 from second 6 on.
  assert.deepEqual(measured(changed(second => ({ keyFramesDecoded: (second >= 10 ? second - 10 : second) * 2 - (second >= 5 && second < 10 ? 2 : 0) })), 'keyframesPerSecond', { min: 1.5 }),
    [[5000, 1.33]]);
  assert.deepEqual(measured(changed(second => ({ keyFramesDecoded: second >= 10 ? (second - 10) * 2 : second * 2 - Math.max(0, second - 3) })), 'keyframesPerSecond', { min: 1.5 }),
    [[5000, 1.33]]);
  assert.deepEqual(measured(changed(second => ({ framesDropped: second >= 4 && second < 10 ? 3 : 0 })), 'browserFramesDropped', { max: 0 }), [[4000, 3]]);
  assert.deepEqual(measured(changed(second => ({ pliCount: second >= 14 ? 1 : 0 })), 'keyframeRequests', { max: 0 }), [[14_000, 1]]);
  // Another SSRC within the second session is a replaced stream; the counts of the new stream are not compared with the old one's.
  const replaced = changed(second => (second >= 15 ? { ssrc: 2, framesDropped: 7 } : {}));
  assert.deepEqual(measured(replaced, 'videoStreamReplaced', { max: 0 }), [[15_000, 1]]);
  assert.deepEqual(measured(replaced, 'browserFramesDropped', { max: 0 }), []);
  // One session of 20 s in which the browser decodes 27 of the 30 frames from second 8 on: the share of the last 10 s is
  // 291 of 300 at second 10, 285 at second 12, and under 95 % from second 13 on.
  const slow = recording(20, { video: true }).map((row, second) => ({ ...row, device: { ...row.device, accessUnitsSent: second * 30 },
    browser: { connectionState: 'connected', peer: 1, video: { id: 'IT01V1', ssrc: 1, timestamp: T0 + second * 1000,
      framesDecoded: second * 30 - Math.max(0, second - 7) * 3 } } }));
  assert.deepEqual(measured(slow, 'framesDecodedShare', { min: 0.95 }), [[13_000, 0.94]]);
});

test('the keyframe rate and the decoded share are judged only when their whole window lies in states of their rule', () => {
  // Packet loss from second 10 to 14, which the rules leave out: the browser decodes 10 of the 30 frames and no keyframe
  // in those seconds, and all of them before and after, in the same session and stream.
  const lossy = second => second >= 10 && second < 15;
  const rows = recording(40, second => ({ video: true, packetLoss: lossy(second) })).map((row, second) => {
    const lost = Math.min(5, Math.max(0, second - 9));
    const clean = second + 1 - lost;
    return { ...row, device: { ...row.device, accessUnitsSent: (second + 1) * 30 },
      browser: { connectionState: 'connected', peer: 'a.1', video: { id: 'IT01V1', ssrc: 1, timestamp: T0 + second * 1000,
        framesDecoded: clean * 30 + lost * 10, keyFramesDecoded: clean * 2, framesDropped: 0, pliCount: 0 } } };
  });
  const criteria = { everySecond: [{ while: { video: true, packetLoss: false }, measure: 'keyframesPerSecond', min: 1.5 },
    { while: { video: true, packetLoss: false }, measure: 'framesDecodedShare', min: 0.95 }] };
  assert.deepEqual(judge(rows, criteria).failures.map(f => [f.measure, f.t - T0]), []);
  // The rule judges the same seconds when the loss is in a state that it judges as well.
  const judged = rows.map(row => ({ ...row, state: { video: true } }));
  assert.deepEqual([...new Set(judge(judged, criteria).failures.map(f => f.measure))].sort(), ['framesDecodedShare', 'keyframesPerSecond']);
});

test('the keyframe rate and the decoded share are not judged while their window has a part of a switch that pauses them', () => {
  // Service video ends at 4.5 s; the switch is done at second 15, with no frame of HDMI capture until then. From then on
  // 30 frames and 2 keyframes per second, in the same session and stream.
  const state = second => ({ video: true, osVideo: second < 5 });
  const rows = recording(40, state).map((row, second) => {
    const running = Math.max(0, second - 15);
    return { ...row, device: { ...row.device, accessUnitsSent: Math.min(second, 5) * 30 + running * 30 },
      browser: { connectionState: 'connected', peer: 'a.1', video: { id: 'IT01V1', ssrc: 1, timestamp: T0 + second * 1000,
        framesDecoded: Math.min(second, 5) * 30 + running * 30, keyFramesDecoded: running * 2, framesDropped: 0, pliCount: 0 } } };
  });
  const criteria = { everySecond: [{ while: { video: true, osVideo: false }, measure: 'keyframesPerSecond', min: 1.5 },
    { while: { video: true, osVideo: false }, measure: 'framesDecodedShare', min: 0.95 }],
  changeTakesAtMost: [{ change: { osVideo: false }, max: 12, pauses: ['keyframesPerSecond', 'framesDecodedShare'] }] };
  const failed = list => judge(list, criteria).failures.map(f => [f.measure, f.t - T0]);
  assert.deepEqual(failed([{ ...change('osVideo', false, 4.5, 10.5, { state: { video: true } }), switchSeconds: 10.5 }, ...rows]), []);
  // The same seconds without the change's row are a stream without keyframes for 10 s.
  assert.deepEqual(failed(rows).map(([measure]) => measure), ['keyframesPerSecond']);
  // A walk that began with HDMI capture for a second: the rule's state began again during the pause, so the judging
  // begins when the pause ends. From second 26 the browser decodes 20 of the 30 frames, and the share is under 95 % in
  // the window of second 27.
  const slower = rows.map((row, second) => ({ ...row, state: { video: true, osVideo: second >= 1 && second < 5 },
    ...(second < 26 ? {} : { browser: { ...row.browser,
      video: { ...row.browser.video, framesDecoded: row.browser.video.framesDecoded - (second - 25) * 10 } } }) }));
  assert.deepEqual(failed([{ ...change('osVideo', false, 4.5, 10.5, { state: { video: true } }), switchSeconds: 10.5 }, ...slower])
    .filter(([measure]) => measure === 'framesDecodedShare'), [['framesDecodedShare', 27_000]]);
});

test('service video faults and failed switches are what the Mini\'s counts grew by, and new connections after a takeover what the tabs made since the sample before', () => {
  const rows = recording(8, { video: true, osVideo: true, takeover: true }).map((row, second) => ({ ...row,
    device: { ...row.device, guestVideo: { source: 'guest', faults: Number(second >= 3), switchFailures: Number(second >= 5) * 2 } },
    loads: { takeover: { newConnections: second < 2 ? null : second >= 6 ? 3 : 0 } } }));
  assert.deepEqual(values(rows, 'serviceVideoFaults'), [[3000, 1]]);
  assert.deepEqual(values(rows, 'serviceVideoSwitchFailures'), [[5000, 2]]);
  assert.deepEqual(values(rows, 'takeoverNewConnections'), [[6000, 3]]);
});

test('one service video frame that the Mini dropped fails the walk on both boards, also during a change of osVideo', () => {
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const dropped = drops => {
      const rows = recording(8, { video: true, osServices: true, osVideo: true }).map((row, second) => ({ ...row,
        device: { ...row.device, guestVideo: { source: 'guest', framesDropped: drops(second) } } }));
      const switched = change('osVideo', true, 2, 3, { state: { video: true, osServices: true } });
      return judge([...rows.slice(0, 5), switched, ...rows.slice(5)], criteria).failures
        .filter(f => f.measure === 'serviceVideoFramesDropped').map(f => [f.t - T0, f.value]);
    };
    assert.deepEqual(dropped(second => Number(second >= 4)), [[4000, 1]], board);
    assert.deepEqual(dropped(() => 0), [], board);
  }
});

test('a protocol error in the sender\'s log fails the run, and so does a log that could not be read; a log without one does not', () => {
  const rows = recording(5, { video: true, osServices: true });
  const withLog = senderLog => judge([...rows, { t: T0 + 6000, senderLog }], rules());
  assert.equal(withLog({ since: T0, lines: 40, protocolErrors: [] }).result, 'pass');
  const failed = withLog({ since: T0, lines: 40, protocolErrors: ['video.frame payload is truncated', 'reserved byte is 7'] });
  assert.deepEqual([failed.result, failed.outcome, failed.unnamed], ['fail', 'fail', 1]);
  assert.match(report(failed, T0), /FAIL {2}the sender's log has 2 lines of a protocol error\n +video\.frame payload is truncated\n +reserved byte is 7/);
  const unread = withLog({ since: T0, error: 'user@host: sudo: a password is required' });
  assert.equal(unread.result, 'fail');
  assert.match(report(unread, T0), /FAIL {2}the sender's log could not be read: user@host: sudo: a password is required/);
});

test('audioToneHz is the strongest frequency that the browser heard in the second, and has no value while the Mini holds the sound off', () => {
  // The strongest frequency of each window of sound of each second: the middle one counts, the lower one of two,
  // so the beep in one window of a second is not the second's.
  const tones = [[222.7, 996.1, 222.7], [996.1, 222.7], [996.1, 996.1], [222.7], [445.3], []];
  const rows = recording(6, { video: true, audio: true }).map((row, second) => ({ ...row,
    loads: { audio: { tones: tones[second], packetsReceived: second * 50 } } }));
  const limit = rules({ while: { audio: true }, measure: 'audioToneHz', min: 190, max: 250 });
  assert.deepEqual(judge(rows, limit).failures.map(f => [f.t - T0, f.value]), [[2000, 996.1], [4000, 445.3]]);
});

test('the transfers of osFilesSmall are counted as those of osFiles are: failed, closed by the Mini, and corrupt', () => {
  const rows = recording(8, { video: true, osFilesSmall: true });
  rows.forEach((row, second) => {
    row.loads = { osFilesSmall: { bytesSent: second * 20_000, started: second * 5, broken: Number(second >= 3), closed: Number(second >= 3),
      failed: Number(second >= 5), corrupt: Number(second >= 5), refused: Number(second >= 7) * 2 } };
  });
  // A drop that the Mini refused is a failed one of the small transfer, and not of the 1 MiB transfer.
  assert.deepEqual(values(rows, 'filesFailed'), [[3000, 1], [5000, 1], [7000, 2]]);
  const large = rows.map(row => ({ ...row, state: { video: true, osFiles: true }, loads: { osFiles: row.loads.osFilesSmall } }));
  assert.deepEqual(values(large, 'filesFailed'), [[3000, 1], [5000, 1]]);
  assert.deepEqual(values(rows, 'fileTransfersClosed'), [[3000, 1]]);
  assert.deepEqual(values(rows, 'fileTransfersCorrupt'), [[5000, 1]]);
});

test('terminalLineMaxMs is the slowest round trip of the light terminal of each second, a line without an answer is 3 s, and fileTransfersClosed the growth of the closed transfers', () => {
  const rows = recording(8, { video: true, osTerminalLight: true, osFiles: true });
  const trips = [[], [180, 40], [200], [40, 600], [null, 30], [], [190], [210]];
  rows.forEach((row, second) => {
    row.loads = { osTerminalLight: { bytes: second * 400, sent: second * 10, answered: second * 10, roundTrips: trips[second].map(ms => [row.t - 500, ms]) },
      osFiles: { bytesSent: second * 300_000, broken: Number(second >= 2) + Number(second >= 4) + Number(second >= 6),
        closed: Number(second >= 2) + Number(second >= 6), failed: 0 } };
  });
  const limit = rules({ while: { osTerminalLight: true }, measure: 'terminalLineMaxMs', max: 200 });
  assert.deepEqual(judge(rows, limit).failures.map(f => [f.t - T0, f.value]), [[3000, 600], [7000, 210]]);
  // The line without an answer is 3000 ms.
  const answer = rules({ while: { osTerminalLight: true }, measure: 'terminalLineMaxMs', max: 2999 });
  assert.deepEqual(judge(rows, answer).failures.map(f => [f.t - T0, f.value]), [[4000, 3000]]);
  assert.deepEqual(values(rows, 'fileTransfersClosed'), [[2000, 1], [6000, 1]]);
  // A closed transfer is also a failed one, as before, and so is the one of second 4, which a lost connection ended.
  assert.deepEqual(values(rows, 'filesFailed'), [[2000, 1], [4000, 1], [6000, 1]]);
});

test('the terminal counts a number that comes in two messages once, and numbers that did not come as one break, with its time and its two numbers', async () => {
  // Windows sends the numbers in messages of 16 KiB, which end inside a line.
  const channel = { readyState: 'open', send() {}, close() {} };
  const peer = { connectionState: 'connected', createDataChannel: () => channel };
  globalThis.window = { __kvmTestHooks: { _getPeerConnection: () => peer } };
  const page = { evaluate: async (f, arg) => f(arg) };
  const { osTerminal } = osServicesParts({ page, call: null, hostExec: null, host: {} });
  try {
    startTerminal('count\r');
    const before = Date.now();
    for (const text of ['1\r\n2\r\n10', '00\r\n1001\r', '\n1002\r\n']) {
      channel.onmessage({ data: new TextEncoder().encode(text).buffer });
    }
    const { lines, breaks } = await osTerminal.sample();
    assert.equal(lines, 5);
    assert.deepEqual(breaks.map(([at, ...numbers]) => [at >= before && at <= Date.now(), ...numbers]), [[true, 2, 1000]]);
    // Each break is in one sample.
    channel.onmessage({ data: new TextEncoder().encode('1003\r\n1005\r\n').buffer });
    assert.deepEqual((await osTerminal.sample()).breaks.map(([, ...numbers]) => numbers), [[1003, 1005]]);
    assert.deepEqual((await osTerminal.sample()).breaks, []);
  } finally {
    delete globalThis.window;
  }
});

for (const system of ['Linux', 'Darwin']) {
  test(`what the ${system} host types into the terminal prints every number from 1 past a million, one per line`, () => {
    // The command as the host's shell gets it, with a pipe in place of the
    // terminal and the first 7.5 MB of its output, which end with 1074672
    // and have a line with the host's clock before each write.
    const typed = hostFor(system).countInTerminal;
    assert.ok(typed.endsWith('\r'));
    const { stdout, status } = spawnSync('sh', ['-c', `${typed.slice(0, -1)} | head -c 7500000`], { maxBuffer: 16 * 1024 * 1024 });
    assert.equal(status, 0);
    assert.equal(stdout.length, 7500000);
    const channel = { readyState: 'open', send() {}, close() {} };
    const peer = { connectionState: 'connected', createDataChannel: () => channel };
    globalThis.window = { __kvmTestHooks: { _getPeerConnection: () => peer } };
    try {
      startTerminal('count\r');
      // In messages of 8192 bytes, which end inside a line.
      for (let at = 0; at < stdout.length; at += 8192) {
        channel.onmessage({ data: new Uint8Array(stdout.subarray(at, at + 8192)).buffer });
      }
      // The line with the host's clock before each write is not a number.
      const { lines, breaks, last, bytes } = globalThis.window.__combinedLoadTerminal;
      assert.equal(bytes, 7500000);
      assert.deepEqual(breaks, []);
      assert.deepEqual({ lines, last }, { lines: 1074672, last: 1074672 });
    } finally {
      delete globalThis.window;
    }
  });
}

test('the terminal counts a number whose line ends in two carriage returns', () => {
  // macOS: "292\r\r\n" between "291\r\n" and "293\r\n".
  const channel = { readyState: 'open', send() {}, close() {} };
  const peer = { connectionState: 'connected', createDataChannel: () => channel };
  globalThis.window = { __kvmTestHooks: { _getPeerConnection: () => peer } };
  try {
    startTerminal('count\r');
    channel.onmessage({ data: new TextEncoder().encode('291\r\n292\r\r\n293\r\n').buffer });
    const { lines, breaks } = globalThis.window.__combinedLoadTerminal;
    assert.deepEqual({ lines, breaks }, { lines: 3, breaks: [] });
  } finally {
    delete globalThis.window;
  }
});

// A directory with stand-ins for the programs of a Windows host that has a
// sender installed. Each notes its arguments in "calls" when it returns. The
// sender's --stop takes 300 ms and exits with `stopStatus`.
function windowsWithSender(stopStatus) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const stub = (file, script) => {
    fs.mkdirSync(path.dirname(path.join(directory, file)), { recursive: true });
    fs.writeFileSync(path.join(directory, file), `#!/bin/sh\n${script}\n`, { mode: 0o755 });
  };
  const note = name => `echo "${name} $*" >> "$HOME/calls"`;
  stub('System32/whoami.exe', 'echo \'"host\\user","S-1-5-21-7"\'');
  stub('schtasks', note('schtasks'));
  stub('taskkill', note('taskkill'));
  stub('local/IP-KVM OS Services/ipkvm-os-services.exe',
    `if [ "$1" = --stop ]; then sleep 0.3; ${note('sender')}; echo usage >&2; exit ${stopStatus}; fi; ${note('sender')}`);
  const env = { PATH: `${directory}:/usr/bin:/bin`, HOME: directory, SYSTEMROOT: directory, LOCALAPPDATA: path.join(directory, 'local') };
  const run = command => spawnSync('sh', ['-c', command], { encoding: 'utf8', env });
  const calls = () => fs.readFileSync(path.join(directory, 'calls'), 'utf8').trim().split('\n');
  return { directory, stub, run, calls };
}
const WINDOWS_ENDED = ['schtasks /end /tn IP-KVM OS Services S-1-5-21-7', 'taskkill /f /im ipkvm-os-services.exe'];

test('on Windows the task and the process are ended only after the sender has answered --stop', () => {
  for (const stopStatus of [0, 1]) {
    const host = windowsWithSender(stopStatus);
    const result = host.run(hostFor('MINGW64_NT-10.0').stopSender);
    assert.deepEqual({ status: result.status, output: result.stdout + result.stderr }, { status: 0, output: '' });
    assert.deepEqual(host.calls(), ['sender --stop', ...WINDOWS_ENDED]);
  }
});

test('on Windows a host without a sender has its task and process ended', () => {
  const host = windowsWithSender(0);
  fs.rmSync(path.join(host.directory, 'local'), { recursive: true });
  assert.equal(host.run(hostFor('MINGW64_NT-10.0').stopSender).status, 0);
  assert.deepEqual(host.calls(), WINDOWS_ENDED);
});

test('on Windows the install goes on over a sender that has no --stop', () => {
  const host = windowsWithSender(1);
  // The installer is on the host already, so the install downloads nothing.
  const installer = '.local/share/jetkvm-combined-load/setup.exe';
  host.stub(installer, 'echo "installer $*" >> "$HOME/calls"');
  const sha256 = createHash('sha256').update(fs.readFileSync(path.join(host.directory, installer))).digest('hex');
  const result = host.run(hostFor('MINGW64_NT-10.0').installSender({ ...SENDER, sha256, file: 'setup.exe' }));
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(host.calls(), ['sender --stop', ...WINDOWS_ENDED, 'installer /S',
    'sender --approve-kvm AA:BB:CC:DD', 'sender --install-service', 'sender --stop', ...WINDOWS_ENDED]);
});

test('a line of a key watch is a report with the host\'s time of a move, and a key and a beat have no time', () => {
  assert.deepEqual(parseReport('m16004 1791557206217.950', 5000), ['m16004', 1791557206217.95, 5000]);
  assert.deepEqual(parseReport('p4', 5000), ['p4', null, 5000]);
  assert.deepEqual(parseReport('p39\r', 5000), ['p39', null, 5000]);
  assert.deepEqual(parseReport('.', 5000), ['.', null, 5000]);
  assert.deepEqual(parseReport('m16004\r', 5000), ['m16004', null, 5000]);
  // A key without its usage is no report.
  assert.equal(parseReport('p', 5000), null);
  assert.equal(parseReport('swift: warning', 5000), null);
  assert.equal(parseReport('', 5000), null);
});

test('the keys are a to z and 0 to 9, each with its HID usage, its Linux key code and its Windows virtual-key code', () => {
  assert.equal(HID_KEYS.map(key => key.name).join(''), 'abcdefghijklmnopqrstuvwxyz0123456789');
  for (const code of ['usage', 'linux', 'windows']) assert.equal(new Set(HID_KEYS.map(key => key[code])).size, 36, code);
  const byName = name => HID_KEYS.find(key => key.name === name);
  // The codes of the HID usage tables, of linux/input-event-codes.h and of WinUser.h.
  assert.deepEqual(byName('a'), { name: 'a', usage: 0x04, linux: 30, windows: 0x41 });
  assert.deepEqual(byName('m'), { name: 'm', usage: 0x10, linux: 50, windows: 0x4d });
  assert.deepEqual(byName('z'), { name: 'z', usage: 0x1d, linux: 44, windows: 0x5a });
  assert.deepEqual(byName('1'), { name: '1', usage: 0x1e, linux: 2, windows: 0x31 });
  assert.deepEqual(byName('9'), { name: '9', usage: 0x26, linux: 10, windows: 0x39 });
  assert.deepEqual(byName('0'), { name: '0', usage: 0x27, linux: 11, windows: 0x30 });
});

test('the page keeps each message of the Mini that lists the keys that are down, from the HID channel and from the RPC channel', async t => {
  // A page's peer connection with the channels that the page makes.
  class FakeChannel {
    constructor(label) { this.label = label; this.listeners = []; }
    addEventListener(type, listener) { if (type === 'message') this.listeners.push(listener); }
    receive(data) { return Promise.all(this.listeners.map(listener => listener({ data }))); }
  }
  class FakePeer {}
  FakePeer.prototype.createDataChannel = label => new FakeChannel(label);
  globalThis.window = {};
  globalThis.RTCPeerConnection = FakePeer;
  let clock = 2000;
  t.mock.method(Date, 'now', () => clock);
  try {
    watchKeyStates();
    const peer = new FakePeer();
    const hid = peer.createDataChannel('hidrpc');
    const rpc = peer.createDataChannel('rpc');
    const other = peer.createDataChannel('hidrpc-unreliable-ordered');
    assert.ok(hid instanceof FakeChannel);
    // a down as an ArrayBuffer, then all keys up as a Blob, then another message of the HID channel.
    await hid.receive(new Uint8Array([0x33, 0, 0x04, 0, 0, 0, 0, 0]).buffer);
    clock = 2005;
    await hid.receive(new Blob([new Uint8Array([0x33, 0, 0, 0, 0, 0, 0, 0])]));
    await hid.receive(new Uint8Array([0x32, 1]).buffer);
    // Before the handshake: the notification on the RPC channel, beside an answer.
    clock = 2010;
    await rpc.receive(JSON.stringify({ jsonrpc: '2.0', method: 'keysDownState', params: { modifier: 0, keys: [0x27] } }));
    await rpc.receive(JSON.stringify({ jsonrpc: '2.0', id: 1, result: { keysDownState: 'no' } }));
    await rpc.receive('not json keysDownState');
    // Another channel is not watched.
    await other.receive(new Uint8Array([0x33, 0, 0x05, 0, 0, 0, 0, 0]).buffer);
    assert.deepEqual(window.__combinedLoadKeyStates,
      [[2000, [0x04, 0, 0, 0, 0, 0]], [2005, [0, 0, 0, 0, 0, 0]], [2010, [0x27]]]);
  } finally {
    delete globalThis.window;
    delete globalThis.RTCPeerConnection;
  }
});

test('the page holds a key until the Mini\'s message lists it, and releases it after the hold at the latest', async t => {
  class FakeChannel {
    constructor(label) { this.label = label; this.listeners = []; }
    addEventListener(type, listener) { if (type === 'message') this.listeners.push(listener); }
    receive(data) { return Promise.all(this.listeners.map(listener => listener({ data }))); }
  }
  class FakePeer {}
  FakePeer.prototype.createDataChannel = label => new FakeChannel(label);
  // The page's handler of a key notes each press and release; the page's timers are the test's.
  const sent = [];
  const timers = [];
  globalThis.window = { __kvmTestHooks: { sendKeypress: (usage, press) => sent.push([usage, press]) } };
  globalThis.RTCPeerConnection = FakePeer;
  t.mock.method(Date, 'now', () => 3000);
  const real = { setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout };
  globalThis.setTimeout = (callback, ms) => timers.push({ callback, ms, cleared: false }) - 1;
  globalThis.clearTimeout = id => { if (timers[id]) timers[id].cleared = true; };
  const state = keys => new Uint8Array([0x33, 0, ...keys]).buffer;
  try {
    watchKeyStates(500);
    const hid = new FakePeer().createDataChannel('hidrpc');
    const keys = window.__combinedLoadKeys;
    // a and b are down; a message that lists a releases a at once, and b stays down.
    assert.equal(keys.press(0x04), 3000);
    keys.press(0x05);
    assert.deepEqual(sent, [[0x04, true], [0x05, true]]);
    await hid.receive(state([0x04, 0]));
    assert.deepEqual(sent.slice(2), [[0x04, false]]);
    assert.deepEqual(timers.map(timer => [timer.ms, timer.cleared]), [[500, true], [500, false]]);
    // b has no message: it goes up after its 500 ms, once, also when a later message lists it.
    timers[1].callback();
    await hid.receive(state([0x05]));
    assert.deepEqual(sent.slice(3), [[0x05, false]]);
    // c, and a release of every key that is down, as when the switch goes off.
    keys.press(0x06);
    keys.releaseAll();
    keys.releaseAll();
    assert.deepEqual(sent.slice(4), [[0x06, true], [0x06, false]]);
  } finally {
    Object.assign(globalThis, real);
    delete globalThis.window;
    delete globalThis.RTCPeerConnection;
  }
});

test('a key\'s latency is the time from its send to the first message after it that lists it, and a key without one is judged once after 3 s', () => {
  const keys = keyAnswers();
  // a: answered after 6 ms; its key-up message came in first.
  keys.send(0x04, 10_000);
  keys.states([[10_007, [0, 0, 0, 0, 0, 0]], [10_006, [0x04, 0, 0, 0, 0, 0]]]);
  assert.deepEqual(keys.judge(10_050), [[0x04, 10_000, 6]]);
  // b: a message that lists it before its send, and one of another key, do not answer it.
  keys.send(0x05, 10_100);
  keys.states([[10_090, [0x05]], [10_103, [0x06]]]);
  assert.deepEqual(keys.judge(10_150), []);
  // c is sent; b has no answer yet.
  keys.send(0x06, 10_200);
  assert.deepEqual(keys.judge(10_200), []);
  // b is answered after 150 ms: its latency counts.
  keys.states([[10_250, [0x05]]]);
  assert.deepEqual(keys.judge(10_300), [[0x05, 10_100, 150]]);
  // c is answered after 2.5 s, which counts.
  keys.states([[12_700, [0x06]]]);
  assert.deepEqual(keys.judge(12_750), [[0x06, 10_200, 2500]]);
  // d has no answer 3 s after its send.
  keys.send(0x07, 13_000);
  assert.deepEqual(keys.judge(15_999), []);
  assert.deepEqual(keys.judge(16_000), [[0x07, 13_000, null]]);
  // A message after that answers nothing.
  keys.states([[16_100, [0x07]]]);
  assert.deepEqual(keys.judge(16_200), []);
});

test('a key is lost when the tool has not read the host\'s report of it within 3 s of its send', () => {
  const host = hostKeys();
  host.send(0x04, 1000);
  host.report(0x04, 1010);
  host.send(0x05, 1100);
  // A report of 0x06 that the tool read before its send is of an earlier press.
  host.send(0x06, 1200);
  host.report(0x06, 1190);
  // A report of 0x07 read 3 s after its send counts; 0x08's, 1 ms later, does not.
  host.send(0x07, 1300);
  host.report(0x07, 4300);
  host.send(0x08, 1400);
  host.report(0x08, 4401);
  assert.deepEqual(host.judge(4099), []);
  assert.deepEqual(host.judge(4100), [{ usage: 0x05, sentAt: 1100 }]);
  assert.deepEqual(host.judge(4400), [{ usage: 0x06, sentAt: 1200 }, { usage: 0x08, sentAt: 1400 }]);
  // Each key is judged once.
  assert.deepEqual(host.judge(10_000), []);
});

// The hand's moves of one movement: 30 moves 33 ms apart from `from` on,
// each to its own place, as hostMoves gets them. The host, whose clock is
// 5000 ms behind this machine's, has a move after host(n) ms or never for
// null, and the tool reads its line 2 ms later.
const AHEAD = 5000;
function movement(mouse, number, from, host) {
  for (let n = 0; n < 30; n++) {
    const at = from + n * 33;
    const x = 4000 + (number * 30 + n) * 24;
    mouse.send(x, at, number);
    const after = host(n);
    if (after !== null) mouse.report(x, at + after - AHEAD, at + after + 2);
  }
}

test('a move is lost when the host has not reported it a second and the grace after its send, and the gap is the longest time between two moves of a movement on the host', () => {
  const mouse = hostMoves();
  // The host has each move after 6 ms, but not the move 10, and the moves
  // 15 to 19 only with the move 20: the pointer stands still on the host
  // from the move 14 to the move 15, for 6 moves of 33 ms.
  movement(mouse, 0, 1000, n => (n === 10 ? null : n >= 15 && n < 20 ? 6 + (20 - n) * 33 : 6));
  // A move is judged when it is 1 s and the grace of 300 ms old.
  assert.deepEqual(mouse.judge(2299), { mouseLost: 0, mouseGapMs: null, gapAt: null });
  assert.deepEqual(mouse.judge(2300 + 12 * 33), { mouseLost: 1, mouseGapMs: 66, gapAt: 1000 + 11 * 33 });
  assert.deepEqual(mouse.judge(2300 + 29 * 33), { mouseLost: 1, mouseGapMs: 198, gapAt: 1000 + 15 * 33 });
  // The next movement, after the hand's rest: the time from the last move of
  // the one before to its first is not a gap.
  movement(mouse, 1, 3000, () => 6);
  assert.deepEqual(mouse.judge(4300 + 29 * 33), { mouseLost: 1, mouseGapMs: 33, gapAt: 3000 + 1 * 33 });
  // A call that judges no move has no gap.
  assert.deepEqual(mouse.judge(5300), { mouseLost: 1, mouseGapMs: null, gapAt: null });
});

test('a report of a place from before a move to it is not the report of that move, and a line that comes late still counts', () => {
  const mouse = hostMoves();
  // The host reports the place 4024 by itself, and 10 ms later the page sends a move there, which is lost.
  mouse.report(4024, 990 - AHEAD, 992);
  mouse.send(4024, 1000, 0);
  // A move whose line reaches the tool 800 ms after the send.
  mouse.send(4048, 1033, 0);
  mouse.report(4048, 1039 - AHEAD, 1833);
  assert.deepEqual(mouse.judge(2400), { mouseLost: 1, mouseGapMs: null, gapAt: null });
  // A place that is used again, 67 s later: the first move's report is not the second move's.
  mouse.send(4072, 3000, 1);
  mouse.report(4072, 3006 - AHEAD, 3008);
  mouse.judge(4400);
  mouse.send(4072, 70_000, 33);
  assert.deepEqual(mouse.judge(71_400), { mouseLost: 2, mouseGapMs: null, gapAt: null });
});

// A recording of 20 seconds in which the hand moves in the even seconds and
// rests in the odd ones; each move and each line of the host's watch is in
// the sample of its second. host(at) is the time after which the host has
// the move sent at `at`, or null for never.
function movesRecording(host) {
  const moves = [];
  const reports = [];
  for (let second = 0; second < 20; second += 2) {
    for (let n = 0; n < 30; n++) {
      const at = T0 + second * 1000 + n * 33 - 1000;
      const x = 4000 + ((second * 15 + n) % 1000) * 24;
      moves.push([x, at, second / 2]);
      const after = host(at);
      if (after !== null) reports.push([`m${x}`, at + after - AHEAD, at + after + 2]);
    }
  }
  return withHid(recording(20, { hid: true }), { moves, reports });
}
// The host gets the moves sent from `from` to `to` ms after T0 all at `to`, and the others after 6 ms.
const stalled = (from, to) => at => (at >= T0 + from && at < T0 + to ? T0 + to - at + 6 : 6);

test('on both boards the pointer may stand still on the host for 500 ms while the hand moves, and every move reaches the host', () => {
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const failed = rows => judge(rows, criteria).failures.map(f => [f.measure, (f.t - T0) / 1000, f.value]);
    // The hand rests for a second between its movements, which is no gap.
    assert.deepEqual(failed(movesRecording(() => 6)), [], board);
    // The moves of the hand's movement in second 3, from 3.3 s on, stand still for 400 ms or 600 ms.
    assert.deepEqual(failed(movesRecording(stalled(3300, 3700))), [], board);
    assert.deepEqual(failed(movesRecording(stalled(3300, 3900))), [['mouseGapMs', 5, 603]], board);
    // One move that never comes is lost, a second and the grace after its send.
    assert.deepEqual(failed(movesRecording(at => (at === T0 + 3000 + 5 * 33 ? null : 6))), [['mouseMovesLost', 5, 1]], board);
  }
});

test('the moves that a change loses or holds up are not counted against the seconds after it, and the moves after it are', () => {
  const criteria = { everySecond: [{ while: { hid: true }, measure: 'mouseMovesLost', max: 0 }, { while: { hid: true }, measure: 'mouseGapMs', max: 500 }],
    changeTakesAtMost: [{ change: { mount: true }, max: 7, pauses: ['mouseMovesLost', 'mouseGapMs'] }] };
  // The mount takes from 10 s to 11.7 s; the hand moves from 9 s to 10 s and from 11 s to 12 s.
  const run = host => judge([change('mount', true, 10, 1.7), ...movesRecording(host)], criteria);
  assert.equal(run(at => (at >= T0 + 11_000 && at < T0 + 11_700 ? null : 6)).result, 'pass');
  assert.equal(run(stalled(11_100, 11_700)).result, 'pass');
  // The same in the next movement, from 13 s on.
  assert.deepEqual(run(at => (at >= T0 + 13_000 && at < T0 + 13_100 ? null : 6)).failures.map(f => [f.measure, f.t - T0]),
    [['mouseMovesLost', 15_000]]);
  assert.deepEqual(run(stalled(13_100, 13_700)).failures.map(f => [f.measure, f.t - T0]), [['mouseGapMs', 15_000]]);
});

test('a recording gives each key with its latency, the keys that the host did not get, the lost moves and the gaps on the host', () => {
  // hid from 11 s to 16 s: a and b at 10.0 s and 10.1 s, and again 3.6 s
  // later; the host does not report the first b. Moves of one movement,
  // whose third the host does not report.
  const sample = (t, sends, keys, moves, reports) => ({ t, state: { hid: true }, loads: { hid: { sends, keys, moves, reports } } });
  const rows = [
    { t: 1, run: {} },
    sample(11_000, [[0x04, 10_000], [0x05, 10_100]], [[0x04, 10_000, 5], [0x05, 10_100, 6]],
      [[4000, 10_000, 0], [4024, 10_033, 0], [4048, 10_066, 0], [4072, 10_099, 0]],
      [['p4', null, 10_010], ['m4000', 5006, 10_008], ['m4024', 5039, 10_041], ['m4072', 5105, 10_107], ['.', null, 10_500]]),
    sample(12_000, [], [], [], []),
    { t: 12_500, change: { mount: true }, state: { hid: true, mount: true }, startedAt: 12_000, seconds: 0.5 },
    sample(13_000, [], [], [], []),
    sample(14_000, [[0x04, 13_600], [0x05, 13_700]], [[0x04, 13_600, null]], [], [['p4', null, 13_612], ['p5', null, 13_712]]),
    sample(15_000, [], [[0x05, 13_700, 7]], [], []),
    // hid off and on again: a new count.
    { t: 16_000, state: { hid: false } },
    sample(17_000, [[0x05, 16_000]], [[0x05, 16_000, 4]], [], []),
    { t: 19_000, state: { hid: false } },
  ];
  const [first, second] = hidTimes(rows);
  assert.deepEqual([first.from, first.to, second.from, second.to], [11_000, 15_000, 17_000, 17_000]);
  assert.deepEqual(first.keys.map(key => [key.usage, key.latencyMs]), [[0x04, 5], [0x05, 6], [0x04, null], [0x05, 7]]);
  assert.deepEqual(first.lostKeys, [{ usage: 0x05, sentAt: 10_100 }]);
  assert.equal(first.moves, 4);
  // The moves of 10.0 s to 10.1 s are judged in the sample of 12 s.
  assert.deepEqual(first.lostMoves, [{ t: 12_000, count: 1 }]);
  assert.deepEqual(first.gaps, [{ gapMs: 66, at: 10_099 }]);
  // The key of the second time that the host never reports is judged in its last sample, as the sample after
  // that time is 3 s after its send.
  assert.deepEqual(second.lostKeys, [{ usage: 0x05, sentAt: 16_000 }]);
});

test('the judge counts the lost keys of each sample from its sends and the host\'s lines, 3 s after the send', () => {
  // a, b and c from 1.0 s on, 100 ms apart, and again 3.6 s later. The host reports a and c each time, and b
  // only the second time.
  const sends = [0x04, 0x05, 0x06].flatMap((usage, i) => [[usage, T0 + 1000 + i * 100], [usage, T0 + 4600 + i * 100]]);
  const reports = sends.filter(([usage, at]) => usage !== 0x05 || at > T0 + 4000).map(([usage, at]) => [`p${usage}`, null, at + 15]);
  const lost = rows => [...hostInputAt(rows).values()].map(entry => entry.keysLost ?? null);
  // The first b is judged at 4.1 s. The first sample has no count: nothing before it says what was there
  // before its second.
  assert.deepEqual(lost(withHid(recording(8, { video: true, hid: true }), { sends, reports })), [null, 0, 0, 0, 0, 1, 0, 0]);
  // A line of b that the tool read at 4.5 s, 3.4 s after the first b and before the second, is the report of
  // neither: both are lost.
  const late = withHid(recording(10, { video: true, hid: true }), { sends,
    reports: [...reports.filter(([what]) => what !== 'p5'), ['p5', null, T0 + 4500]] });
  assert.deepEqual(lost(late), [null, 0, 0, 0, 0, 1, 0, 0, 1, 0]);
  // A line that the tool read in the same millisecond as the send is the report of that send.
  const same = withHid(recording(10, { video: true, hid: true }), { sends,
    reports: [...reports.filter(([what]) => what !== 'p5'), ['p5', null, T0 + 1100]] });
  assert.deepEqual(lost(same), [null, 0, 0, 0, 0, 0, 0, 0, 1, 0]);
});

test('the last sample of hid judges a key only when its 3 s have passed by the newest time that the recording has', () => {
  const lost = rows => [...hostInputAt(rows).values()].map(entry => entry.keysLost ?? null);
  // A key sent at 0.99 s and reported at 1.04 s. A recording that ends at 1.0 s with hid on, as the settling
  // check judges it, does not count it; the whole recording has its report.
  const reported = { sends: [[0x04, T0 + 990]], reports: [['p4', null, T0 + 1040]] };
  assert.deepEqual(lost(withHid(recording(2, { video: true, hid: true }), reported)), [null, 0]);
  assert.deepEqual(lost(withHid(recording(3, { video: true, hid: true }), reported)), [null, 0, 0]);
  // A key that the host never reports, sent at 3.0 s, 1 s before the last sample of hid. With the sample in
  // which hid is off at 6.0 s, 3 s after the send, it is lost.
  const rows = withHid(recording(5, { video: true, hid: true }), { sends: [[0x04, T0 + 3000]] });
  const offAt = t => [...rows, { t, state: { video: true, hid: false }, loads: {} }];
  assert.deepEqual(lost(offAt(T0 + 6000)), [null, 0, 0, 0, 1]);
  assert.deepEqual(hidTimes(offAt(T0 + 6000))[0].lostKeys, [{ usage: 0x04, sentAt: T0 + 3000 }]);
  // When the recording ends before 6.0 s, the key is not judged.
  assert.deepEqual(lost(offAt(T0 + 5000)), [null, 0, 0, 0, 0]);
  assert.deepEqual(lost(rows), [null, 0, 0, 0, 0]);
  assert.deepEqual(hidTimes(rows)[0].lostKeys, []);
});

test('the events of the first sample of a switch have no value, as a count that grows has none there', () => {
  // A break of the terminal's numbers in its first sample and in its third.
  const rows = recording(4, { video: true, osTerminal: true }).map((row, second) =>
    ({ ...row, loads: { osTerminal: { bytes: second * 1000, lines: second * 10, breaks: second % 2 === 0 ? [[row.t - 500, 41, 43]] : [] } } }));
  assert.deepEqual(valueEachSecond(rows, 'terminalLinesLost'), [null, 0, 1, 0]);
});

test('other times of the judge give other numbers from the same recorded moves', () => {
  // One movement of 30 moves from 3.0 s on, 33 ms apart. The host has the first three moves 6 ms after their
  // send, and the others all at once at 4.2 s: the pointer stands still from the third move on. The tool
  // reads each line 2 ms after the host's time.
  const moves = [];
  const reports = [];
  for (let n = 0; n < 30; n++) {
    const at = T0 + 3000 + n * 33;
    const x = 4000 + n * 24;
    moves.push([x, at, 0]);
    const hostAt = n < 3 ? at + 6 : T0 + 4206;
    reports.push([`m${x}`, hostAt - AHEAD, hostAt + 2]);
  }
  const rows = withHid(recording(8, { video: true, hid: true }), { moves, reports });
  const each = (times, measure) => [...hostInputAt(rows, times).values()].map(entry => entry[measure] ?? null);
  // With the walk's times a move is judged 1.3 s after its send: none is lost; the stop of the pointer, from
  // the move of 3.066 s, which the host had at 3.072 s, to 4.206 s, is in the sample of 5 s, and the moves that
  // came at once have no time between them.
  assert.deepEqual(each({}, 'mouseMovesLost'), [null, 0, 0, 0, 0, 0, 0, 0]);
  assert.deepEqual(each({}, 'mouseGapMs'), [null, null, null, null, null, 1134, 0, null]);
  // A judge that takes a move as lost 200 ms after its send, with the same grace, judges the moves sent up to
  // 3.5 s in the sample of 4 s: the 13 of them that the host had not reported by then are lost.
  assert.deepEqual(each({ lostMs: 200 }, 'mouseMovesLost'), [null, 0, 0, 0, 13, 0, 0, 0]);
  // The judge's rules see the numbers of the walk's times.
  const criteria = rules({ while: { hid: true }, measure: 'mouseGapMs', max: 500 }, { while: { hid: true }, measure: 'mouseMovesLost', max: 0 });
  assert.deepEqual(judge(rows, criteria).failures.map(f => [f.measure, f.t - T0, f.value]), [['mouseGapMs', 5000, 1134]]);
});

test('the hand sends 30 moves in a second, 33 ms apart, each to its own place, and rests for a second', t => {
  // The page's side of moveHand: its timer, its two clocks, and the hook that sends a move.
  let tick = null;
  let clock = 50_000;
  const hook = [];
  globalThis.window = { __kvmTestHooks: { sendAbsMouseMove: (x, y, buttons) => hook.push([x, y, buttons]) } };
  t.mock.method(globalThis, 'setInterval', (callback, ms) => { tick = callback; return ms; });
  const cleared = t.mock.method(globalThis, 'clearInterval', () => {});
  t.mock.method(performance, 'now', () => clock);
  t.mock.method(Date, 'now', () => 1_000_000 + clock);
  const HAND = [1000 / 30, 1000, 1000, { first: 4000, step: 24, count: 1000 }, 16000];
  try {
    moveHand(HAND);
    const hand = window.__combinedLoadMouse;
    // The page's timer fires every 8 ms: the first tick of each period of 33 ms sends its move.
    const run = (from, to, everyMs = 8) => { for (clock = 50_000 + from; clock < 50_000 + to; clock += everyMs) tick(); };
    run(0, 2000);
    const first = hand.take();
    assert.equal(first.length, 30);
    assert.deepEqual(first.slice(0, 4), [[4000, 1_050_000, 0], [4024, 1_050_040, 0], [4048, 1_050_072, 0], [4072, 1_050_104, 0]]);
    assert.deepEqual(first.at(-1), [4000 + 29 * 24, 1_050_968, 0]);
    assert.deepEqual(hook.slice(0, 2), [[4000, 16000, 0], [4024, 16000, 0]]);
    assert.equal(hook.length, 30);
    // What was taken is not given again.
    assert.deepEqual(hand.take(), []);
    // A page whose timer fires every 100 ms sends one move per tick, and not the moves it missed.
    run(2000, 4000, 100);
    const slow = hand.take();
    assert.equal(slow.length, 10);
    assert.deepEqual(slow.slice(0, 2), [[4000 + 30 * 24, 1_052_000, 1], [4000 + 31 * 24, 1_052_100, 1]]);
    // After 1000 moves the first place comes again.
    for (let cycle = 2; cycle < 35; cycle++) run(cycle * 2000, cycle * 2000 + 2000);
    const taken = hand.take();
    const places = taken.map(([x]) => x);
    // Each move has the number of its movement: the hand moved 33 more times.
    assert.deepEqual([taken[0][2], taken.at(-1)[2]], [2, 34]);
    assert.equal(places.length, 33 * 30);
    assert.equal(places[1000 - 40], 4000);
    assert.equal(Math.max(...places), 4000 + 999 * 24);
    // A second hand stops the first.
    moveHand(HAND);
    assert.deepEqual(cleared.mock.calls.map(call => call.arguments), [[1000 / 30 / 4]]);
  } finally {
    delete globalThis.window;
  }
});

// One event of a Linux input device, as the kernel gives it to a reader on x86-64 and arm64.
function inputEvent(ms, type, code, value) {
  const event = Buffer.alloc(24);
  event.writeBigInt64LE(BigInt(Math.floor(ms / 1000)), 0);
  event.writeBigInt64LE(BigInt(Math.round((ms % 1000) * 1000)), 8);
  event.writeUInt16LE(type, 16);
  event.writeUInt16LE(code, 18);
  event.writeInt32LE(value, 20);
  return event;
}

test('the Linux watch reports each move of a mouse that sends 250 at once, with the kernel\'s time of each, and the presses of the keys of HID_KEYS only', async () => {
  // The watch as the tool starts it, with named pipes in the place of the
  // Mini's input devices; the second mouse is not there.
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const python = spawnSync('sh', ['-c', 'command -v python3'], { encoding: 'utf8' }).stdout.trim();
  const stub = (name, script) => fs.writeFileSync(path.join(directory, name), `#!/bin/sh\n${script}\n`, { mode: 0o755 });
  stub('sudo', 'shift; exec "$@"');
  stub('python3', `exec ${python} -u -c "$(printf %s "$3" | sed "s|/dev/input/by-id/usb-JetKVM_USB_Emulation_Device-|$HOME/|")"`);
  const devices = {};
  for (const name of ['event-kbd', 'if01-event-mouse']) {
    assert.equal(spawnSync('mkfifo', [path.join(directory, name)]).status, 0);
    devices[name] = fs.openSync(path.join(directory, name), 'r+');
  }
  const watch = spawn('sh', ['-c', hostFor('Linux').watchKeys], { env: { PATH: `${directory}:/usr/bin:/bin`, HOME: directory } });
  try {
    let text = '';
    watch.stdout.setEncoding('utf8');
    watch.stdout.on('data', chunk => { text += chunk; });
    const reports = () => text.split('\n').slice(0, -1).map(line => parseReport(line, 0));
    // 250 moves, each with the report's end as the kernel writes it, 4 ms apart on the kernel's clock.
    const EV_SYN = 0, EV_KEY = 1, EV_ABS = 3, ABS_X = 0, ABS_Y = 1, KEY_F24 = 194, KEY_A = 30, KEY_0 = 11;
    const at = n => 1_791_557_206_000.25 + n * 4;
    fs.writeSync(devices['if01-event-mouse'], Buffer.concat(Array.from({ length: 250 }, (_, n) =>
      Buffer.concat([inputEvent(at(n), EV_ABS, ABS_X, 4000 + n * 24), inputEvent(at(n), EV_ABS, ABS_Y, 16000 + n), inputEvent(at(n), EV_SYN, 0, 0)]))));
    // a pressed and released, F24, which is not a key of HID_KEYS, and 0, which a key repeat follows.
    fs.writeSync(devices['event-kbd'], Buffer.concat([inputEvent(at(300), EV_KEY, KEY_A, 1), inputEvent(at(300), EV_SYN, 0, 0),
      inputEvent(at(301), EV_KEY, KEY_A, 0), inputEvent(at(302), EV_KEY, KEY_F24, 1), inputEvent(at(303), EV_KEY, KEY_0, 1),
      inputEvent(at(304), EV_KEY, KEY_0, 2)]));
    await until(() => reports().filter(report => report?.[0] !== '.').length >= 252 && reports().some(report => report?.[0] === '.'), 10_000,
      `the watch wrote ${text.split('\n').length - 1} lines`);
    const lines = reports();
    assert.ok(lines.every(Boolean), text);
    assert.deepEqual(lines.filter(([what]) => what.startsWith('m')).map(([what, hostMs]) => [what, hostMs]),
      Array.from({ length: 250 }, (_, n) => [`m${4000 + n * 24}`, at(n)]));
    assert.deepEqual(lines.filter(([what]) => what.startsWith('p')).map(([what, hostMs]) => [what, hostMs]), [['p4', null], ['p39', null]]);
    assert.equal(lines.find(([what]) => what === '.')[1], null);
  } finally {
    // The watch ends when it cannot write its next line.
    watch.stdout.destroy();
    watch.kill();
    Object.values(devices).forEach(device => fs.closeSync(device));
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test('a Windows command has no two backslashes in a row, which its way to the host does not keep', () => {
  for (const command of hostCommands(hostFor('MINGW64_NT-10.0'))) assert.doesNotMatch(command, /\\\\/);
});

// A Mini's stored settings behind its JSON-RPC, as the firmware answers, and
// the calls that changed one.
function fakeMini(stored = {}) {
  const PRESETS = [{ name: 'JetKVM Default', edid: '00FFAA' }, { name: 'DELL D2721H', edid: '00ffbb' }];
  const mini = {
    quality: 0, edid: '00ffaa', idle: false, sleepSeconds: -1, jiggler: false, band: 'auto', automaticUpdates: false,
    osServices: { terminal_enabled: true, files_enabled: true, capture_display: '', video_enabled: true, clipboard_enabled: true, resolution: '2560x1440', display_mode: 'extend' },
    mqtt: { enabled: false, broker: 'broker.test', port: 8883, username: 'u', password: 'secret', base_topic: 'kvm', use_tls: true,
      tls_insecure: false, enable_ha_discovery: true, enable_actions: false, debounce_ms: 500 },
    ...stored,
  };
  const calls = [];
  const set = (method, params, change) => { calls.push([method, params]); change(); return null; };
  const methods = {
    getStreamQualityFactor: () => mini.quality,
    setStreamQualityFactor: params => set('setStreamQualityFactor', params, () => { mini.quality = params.factor; }),
    getEDIDPresets: () => PRESETS,
    getEDID: () => mini.edid,
    getHostDisplayIdleMode: () => ({ enabled: mini.idle }),
    setHostDisplayIdleMode: params => set('setHostDisplayIdleMode', params, () => { mini.idle = params.enabled; }),
    // A session is open, so video does not sleep now, whatever is stored.
    getVideoSleepMode: () => ({ supported: true, enabled: false, duration: mini.sleepSeconds }),
    setVideoSleepMode: params => set('setVideoSleepMode', params, () => { mini.sleepSeconds = params.duration; }),
    getOsServicesSettings: () => mini.osServices,
    setOsServicesSettings: params => set('setOsServicesSettings', params, () => {
      mini.osServices = { ...mini.osServices, ...params.settings };
      if (mini.osServices.adaptation === '') delete mini.osServices.adaptation;
    }),
    getJigglerState: () => mini.jiggler,
    setJigglerState: params => set('setJigglerState', params, () => { mini.jiggler = params.enabled; }),
    getMqttSettings: () => ({ ...mini.mqtt }),
    setMqttSettings: params => set('setMqttSettings', params, () => { mini.mqtt = params.settings; }),
    getWifiSettings: () => ({ available: true, band: mini.band }),
    getAutoUpdateState: () => mini.automaticUpdates,
    // The firmware answers with the value that it stored.
    setAutoUpdateState: params => {
      set('setAutoUpdateState', params, () => { mini.automaticUpdates = params.enabled; });
      return mini.automaticUpdates;
    },
  };
  const rpc = async (method, params) => {
    if (mini.without?.includes(method)) throw new Error(`${method}: Method not found`);
    if (mini.failing?.includes(method)) throw new Error(`${method}: timeout`);
    return methods[method](params);
  };
  return { mini, calls, rpc };
}

test('a run sets the stored settings that are not at their default, and the first row has what it found', async () => {
  // A Mini as it comes: nothing is set, and each setting is in the row.
  const fresh = fakeMini();
  assert.deepEqual(await setDefaults(fresh.rpc), {
    streamQualityFactor: { found: 0 }, edid: { found: 'JetKVM Default' }, hostDisplayIdle: { found: false }, videoSleepSeconds: { found: -1 },
    osServicesVideo: { found: SERVICE_VIDEO }, mouseJiggler: { found: false }, mqtt: { found: false }, automaticUpdates: { found: false },
    wifiBand: { found: 'auto' },
  });
  assert.deepEqual(fresh.calls, []);
  // A bench on which somebody chose the High quality, another display, a
  // video sleep, a fixed rate of service video at 1080p and the jiggler.
  const used = fakeMini({ quality: 1, edid: '00FFBB', sleepSeconds: 300, jiggler: true,
    osServices: { terminal_enabled: false, files_enabled: true, capture_display: 'DELL U2720Q', video_enabled: true, clipboard_enabled: true,
      resolution: '1920x1080', display_mode: 'extend', adaptation: 'fixed:4000:30' } });
  const row = await setDefaults(used.rpc);
  assert.deepEqual([row.streamQualityFactor, row.edid, row.videoSleepSeconds, row.mouseJiggler, row.hostDisplayIdle], [
    { found: 1, default: 0, set: true }, { found: 'DELL D2721H', default: 'JetKVM Default', set: false },
    { found: 300, default: -1, set: true }, { found: true, default: false, set: true }, { found: false }]);
  assert.deepEqual(row.osServicesVideo, { found: { capture_display: 'DELL U2720Q', resolution: '1920x1080', display_mode: 'extend', adaptation: 'fixed:4000:30' },
    default: SERVICE_VIDEO, set: true });
  // The EDID is in the row and stays as it is.
  assert.deepEqual(used.calls, [['setStreamQualityFactor', { factor: 0 }], ['setVideoSleepMode', { duration: -1 }],
    ['setOsServicesSettings', { settings: SERVICE_VIDEO }], ['setJigglerState', { enabled: false }]]);
  // The Mini has the defaults afterwards, and the parts of OS Services keep what they had.
  assert.deepEqual([used.mini.quality, used.mini.edid, used.mini.sleepSeconds, used.mini.jiggler], [0, '00FFBB', -1, false]);
  assert.deepEqual(used.mini.osServices, { terminal_enabled: false, files_enabled: true, capture_display: '', video_enabled: true, clipboard_enabled: true,
    resolution: '2560x1440', display_mode: 'extend' });
  // A display that is no preset is "custom".
  assert.deepEqual((await setDefaults(fakeMini({ edid: '00ffcc' }).rpc)).edid, { found: 'custom', default: 'JetKVM Default', set: false });
});

test('MQTT goes off with the rest of its settings kept, and the row does not have them', async () => {
  const { mini, calls, rpc } = fakeMini({ mqtt: { enabled: true, broker: 'broker.test', port: 8883, username: 'u', password: 'secret', base_topic: 'kvm',
    use_tls: true, tls_insecure: false, enable_ha_discovery: true, enable_actions: false, debounce_ms: 500 } });
  const row = await setDefaults(rpc);
  assert.deepEqual(row.mqtt, { found: true, default: false, set: true });
  assert.deepEqual(calls, [['setMqttSettings', { settings: { ...mini.mqtt, enabled: false } }]]);
  assert.equal(mini.mqtt.password, 'secret');
  assert.doesNotMatch(JSON.stringify(row), /secret|broker/);
});

test('a setting that the run cannot change is reported, one that the build does not have is marked, and a failed call or a kept value stops the run', async () => {
  // The Wi-Fi band and the EDID: the row has them, and no call changes them.
  const fixedBand = fakeMini({ band: '5g', edid: '00ffbb' });
  const kept = await setDefaults(fixedBand.rpc);
  assert.deepEqual([kept.wifiBand, kept.edid], [{ found: '5g', default: 'auto', set: false }, { found: 'DELL D2721H', default: 'JetKVM Default', set: false }]);
  assert.deepEqual(fixedBand.calls, []);
  // A build without the video sleep and the Wi-Fi settings: the row says so, and the others are set.
  const older = await setDefaults(fakeMini({ quality: 1, without: ['getVideoSleepMode', 'getWifiSettings'] }).rpc);
  assert.deepEqual([older.videoSleepSeconds, older.wifiBand, older.streamQualityFactor], [
    { missing: 'getVideoSleepMode: Method not found' }, { missing: 'getWifiSettings: Method not found' }, { found: 1, default: 0, set: true }]);
  // A call that fails for another reason stops the run: the setting is not known. The Mini's
  // "unavailable" is such a failure too: it also says so when a read of its flash fails.
  await assert.rejects(setDefaults(fakeMini({ failing: ['getJigglerState'] }).rpc), /the Mini's stored mouseJiggler cannot be read: getJigglerState: timeout/);
  const noFlash = fakeMini();
  const rpc2 = (method, params) => (method === 'getHostDisplayIdleMode'
    ? Promise.reject(new Error('getHostDisplayIdleMode: Internal error: Host display idle control unavailable')) : noFlash.rpc(method, params));
  await assert.rejects(setDefaults(rpc2), /the Mini's stored hostDisplayIdle cannot be read: .*unavailable/);
  // The stored video sleep is the answer's duration: "enabled" is false while a session is open.
  const sleeps = fakeMini({ sleepSeconds: 90 });
  assert.deepEqual((await setDefaults(sleeps.rpc)).videoSleepSeconds, { found: 90, default: -1, set: true });
  assert.equal(sleeps.mini.sleepSeconds, -1);
  // A Mini that answers the call and keeps the High quality.
  const stuck = fakeMini({ quality: 1 });
  const rpc = (method, params) => (method === 'setStreamQualityFactor' ? null : stuck.rpc(method, params));
  await assert.rejects(setDefaults(rpc, { waitMs: 20, sleep: () => new Promise(resolve => setTimeout(resolve, 5)) }),
    /the Mini keeps streamQualityFactor at 1 and does not take the default, 0/);
});

test('before a list the drive is a CD-ROM drive, and the first row says when the Mini enumerated again for it', async () => {
  // A Mini whose drive is a disk or a CD-ROM drive: a CD-ROM image for a
  // disk makes it enumerate again, and the drive is then a CD-ROM drive. A
  // CD-ROM drive that the host does not ask enumerates again too, so the
  // row says what the Mini did and not which type the drive was.
  const mini = type => {
    const state = { type, enumerations: 7, medium: false, calls: [] };
    return {
      state,
      enumerations: async () => state.enumerations,
      mount: async () => {
        state.calls.push('mount');
        if (state.type === 'disk') state.enumerations++;
        state.type = 'cdrom';
        state.medium = true;
      },
      unmount: async () => {
        state.calls.push('unmount');
        state.medium = false;
      },
    };
  };
  const disk = mini('disk');
  const found = await cdromDrive(disk);
  assert.deepEqual(found, { enumerated: true });
  assert.deepEqual(disk.state, { type: 'cdrom', enumerations: 8, medium: false, calls: ['mount', 'unmount'] });
  assert.deepEqual(settingLines({ usbDrive: found }),
    ['usbDrive did not take a CD-ROM image as a new medium on the Mini; the Mini enumerated on USB again before the list, and the drive is a CD-ROM drive now']);
  // The second list of a run finds the drive as the first one left it.
  assert.deepEqual(await cdromDrive(disk), { enumerated: false });
  const cdrom = mini('cdrom');
  assert.deepEqual(await cdromDrive(cdrom), { enumerated: false });
  assert.deepEqual(cdrom.state.calls, ['mount', 'unmount']);
  assert.deepEqual(settingLines({ usbDrive: { enumerated: false } }), []);
  // A host that kept a medium of an earlier run: its unmount mounts a disk
  // image to make the Mini enumerate again, which leaves a disk, so the
  // mount and the unmount are made once more.
  const kept = mini('cdrom');
  let leftover = true;
  const unmountKept = async () => {
    await kept.unmount();
    if (leftover) {
      kept.state.enumerations++;
      kept.state.type = 'disk';
      leftover = false;
    }
  };
  assert.deepEqual(await cdromDrive({ ...kept, unmount: unmountKept }), { enumerated: true });
  assert.deepEqual(kept.state, { type: 'cdrom', enumerations: 9, medium: false, calls: ['mount', 'unmount', 'mount', 'unmount'] });
  // A Mini that enumerates at every unmount never has a CD-ROM drive.
  const always = mini('cdrom');
  await assert.rejects(cdromDrive({ ...always, unmount: async () => { always.state.enumerations++; } }), /enumerates on USB again at each unmount/);
  assert.equal(always.state.calls.length, 3);
  // A request for the count that the Mini leaves unanswered is made again.
  const slow = mini('disk');
  let asked = 0;
  const flaky = async () => {
    if (asked++ % 2 === 0) throw new Error('fetch failed');
    return slow.state.enumerations;
  };
  const quick = { waitMs: 200, sleep: () => new Promise(resolve => setTimeout(resolve, 5)) };
  assert.deepEqual(await cdromDrive({ ...slow, enumerations: flaky, ...quick }), { enumerated: true });
  assert.equal(asked, 6);
  // A Mini whose stats have no count of enumerations, or that does not
  // answer: the state is unknown, and the run does not start.
  await assert.rejects(cdromDrive({ ...mini('disk'), enumerations: async () => undefined, ...quick }), /does not report its count of USB enumerations \(undefined\)/);
  await assert.rejects(cdromDrive({ ...mini('disk'), enumerations: async () => { throw new Error('fetch failed'); }, ...quick }),
    /does not report its count of USB enumerations \(fetch failed\)/);
  // A mount that fails stops the run's start.
  await assert.rejects(cdromDrive({ ...mini('disk'), mount: async () => { throw new Error('no CD-ROM of the Mini 60 s after the mount'); } }), /no CD-ROM/);
});

test('the report has a line for each stored setting that was not at its default, from the first row', () => {
  const settings = { streamQualityFactor: { found: 1, default: 0, set: true }, edid: { found: 'JetKVM Default' }, wifiBand: { found: '5g', default: 'auto', set: false } };
  assert.deepEqual(settingLines(settings), [
    'streamQualityFactor was 1 on the Mini; the run set the default, 0',
    'wifiBand is "5g" on the Mini, not the default, "auto"; the run does not change it']);
  const rows = first => [{ t: T0, run: { board: 'wifi', host: 'linux', ...first } }, ...recording(10, { video: true })];
  const lines = report(judge(rows({ settings }), rules(VIDEO)), T0).split('\n');
  assert.deepEqual(lines.filter(line => line.startsWith('SETTING')), [
    'SETTING  streamQualityFactor was 1 on the Mini; the run set the default, 0',
    'SETTING  wifiBand is "5g" on the Mini, not the default, "auto"; the run does not change it']);
  assert.equal(lines.at(-1), 'PASS');
  // A Mini at its defaults, and a recording from before the settings were in the row.
  for (const first of [{ settings: { streamQualityFactor: { found: 0 }, wifiBand: { found: 'auto' } } }, {}]) {
    assert.doesNotMatch(report(judge(rows(first), rules(VIDEO)), T0), /SETTING/);
  }
});

// packetLoss with a clock of its own. frames(ms): the frames that the browser
// has decoded `ms` after the end of the loss. missed: what the browser counts
// after the loss, { packetsLost, nackCount }.
function lossWithClock({ frames = ms => Math.floor(ms * 0.03), missed = { packetsLost: 20 } } = {}) {
  let t = 5_000_000;
  let endedAt = null;
  const calls = [];
  const loss = packetLossSwitch({
    emulate: async conditions => {
      calls.push([t, conditions]);
      if (calls.length > 1 && conditions.packetLoss === 0 && endedAt === null) endedAt = t;
    },
    video: async () => ({ framesDecoded: 1000 + (endedAt === null ? 0 : frames(t - endedAt)),
      packetsLost: 3 + (endedAt === null ? 0 : missed.packetsLost ?? 0), nackCount: 7 + (endedAt === null ? 0 : missed.nackCount ?? 0) }),
    now: () => t,
    wait: async ms => { t += ms; },
  });
  return { loss, calls, pass: ms => { t += ms; }, now: () => t, ended: () => endedAt };
}

test('packetLoss sets an emulation without loss before the list, loses every packet for 100 ms, and refuses a loss that the session did not notice', async () => {
  const { loss, calls } = lossWithClock();
  await loss.prepare();
  assert.deepEqual(calls.map(([, conditions]) => conditions), [NO_LOSS]);
  // Chrome counts an emulation as set only with a limit on the rate, and loses nothing at 0 percent.
  assert.ok(NO_LOSS.downloadThroughput > 0 && NO_LOSS.uploadThroughput > 0);
  assert.deepEqual([NO_LOSS.packetLoss, NO_LOSS.latency, NO_LOSS.offline, ALL_LOST.packetLoss], [0, 0, false, 100]);
  assert.equal(await loss.isOn(), false);
  await loss.on();
  assert.deepEqual(calls.slice(1).map(([, conditions]) => conditions), [ALL_LOST, NO_LOSS]);
  assert.equal(calls[2][0] - calls[1][0], DROP_MS);
  assert.equal(await loss.isOn(), true);
  // Off ends the state, and a loss that a stopped run left.
  await loss.off();
  assert.equal(await loss.isOn(), false);
  assert.deepEqual(calls.at(-1)[1], NO_LOSS);

  // The browser asked for the packets again and the Mini sent them: none is counted as lost.
  await lossWithClock({ missed: { nackCount: 20 } }).loss.on();
  // The browser missed nothing: the loss was not on the page's connection.
  const unnoticed = lossWithClock({ missed: {} });
  await assert.rejects(unnoticed.loss.on(), /the page's connection lost no packet.*not set before the page loaded/);
  assert.deepEqual(unnoticed.calls.at(-1)[1], NO_LOSS);
});

// A session with keys in which the link loses packets in second `at`: the
// browser counts 20 video packets as lost and a freeze of 300 ms, decodes 10
// frames less, and two keys are lost.
function lossy(at, state) {
  const rows = recording(40, state);
  let frames = 0;
  rows.forEach((row, second) => {
    frames += second === at ? 20 : 30;
    row.browser.video = { framesDecoded: frames, frameHeight: 1080, packetsLost: second >= at ? 20 : 0,
      totalFreezesDuration: second >= at ? 0.3 : 0 };
  });
  // Two keys that the host did not get, counted 3 s after their send, in the second of `at`.
  const sentAt = T0 + at * 1000 - 3500;
  return withHid(rows, { sends: [[0x04, sentAt], [0x05, sentAt + 100]] });
}

test('on both boards the loss pauses what it interrupts until the session is back, and its limit is the same', () => {
  const files = board => [readJsonc(path.join(here, '..', board, 'criteria.jsonc')), readJsonc(path.join(here, '..', board, 'known-failures.jsonc'))];
  const ruleOf = board => files(board)[0].changeTakesAtMost.find(line => line.change.packetLoss === true);
  assert.deepEqual(ruleOf('wifi'), ruleOf('ethernet'));
  const { max } = ruleOf('ethernet');
  const state = second => ({ video: true, hid: true, packetLoss: second >= 10 && second < 22 });
  const failed = (board, rows) => judge(rows, files(board)[0]).failures.map(f => f.measure ?? `${Object.keys(f.change.change)[0]} took ${f.value}`).sort();
  for (const board of ['ethernet', 'wifi']) {
    // The loss begins at 10 s, and the session is back 2 s later.
    const during = [change('packetLoss', true, 10, 2, { state: { video: true, hid: true } }), ...lossy(11, state)];
    assert.deepEqual(failed(board, during), [], board);
    // The same seconds without the change, and long after it.
    const after = [change('packetLoss', true, 10, 2, { state: { video: true, hid: true } }), ...lossy(30, state)];
    const noticed = ['freezeMs', 'keysLost'];
    assert.deepEqual(failed(board, lossy(11, { video: true, hid: true })), failed(board, after), board);
    assert.deepEqual(failed(board, after).filter(measure => measure !== 'videoPacketsLost'), noticed, board);
    // A session that is back later than the limit.
    const slow = [change('packetLoss', true, 10, max + 1, { state: { video: true, hid: true } }), ...lossy(11, state)];
    assert.deepEqual(failed(board, slow), [`packetLoss took ${max + 1}`], board);
  }
  // Lost packets by themselves: a rule of the Ethernet Mini, and none of the Wi-Fi Mini.
  const lostOnly = recording(40, { video: true });
  lostOnly.forEach((row, second) => { row.browser.video.packetsLost = second >= 20 ? 30 : 0; });
  assert.deepEqual(failed('ethernet', lostOnly), ['videoPacketsLost']);
  assert.deepEqual(failed('wifi', lostOnly), []);
  for (const entry of expand(files('wifi')[1])) assert.notEqual(entry.measure, 'videoPacketsLost', JSON.stringify(entry));
});

test('the report names the seconds with lost video packets, and has them beside a failure of those seconds and of the 5 seconds before and after', () => {
  const FREEZE = { while: { video: true }, measure: 'freezeMs', max: 0 };
  // 12 packets are lost in second 10 and 3 in second 11; video freezes in the seconds of `frozen`.
  const lines = (frozen, { lost = true, changes = [] } = {}) => {
    const rows = recording(40, { video: true });
    rows.forEach((row, second) => {
      row.browser.video.packetsLost = !lost ? 0 : second >= 11 ? 15 : second >= 10 ? 12 : 0;
      row.browser.video.totalFreezesDuration = frozen.filter(at => second >= at).length * 0.2;
    });
    const criteria = { everySecond: [FREEZE], changeTakesAtMost: [{ change: { mount: true }, max: 2 }] };
    return report(judge([...changes, ...rows], criteria), T0).split('\n');
  };
  const beside = (all, start) => all.slice(all.findIndex(line => line.startsWith(start))).filter(line => line.includes('video packets lost'));
  const first = lines([4, 6, 10, 15, 17]);
  assert.equal(first[1], 'LOST  video packets: 12 at 10 s, 3 at 11 s');
  // The browser counts the packets of a time without any packet at its end:
  // the freeze 4 s before the count has it beside it, and the one 6 s before
  // does not. After the loss, the freeze at 15 s has it, and the one at 17 s
  // does not.
  const blocks = first.join('\n').split('\nFAIL  freezeMs').slice(1);
  assert.deepEqual(blocks.map(block => block.split('\n').filter(line => line.includes('video packets lost')).map(line => line.trim())),
    [[], ['video packets lost: 12 at 10 s, 3 at 11 s'], ['video packets lost: 12 at 10 s, 3 at 11 s'],
      ['video packets lost: 12 at 10 s, 3 at 11 s'], []]);
  assert.equal(first.at(-1), 'FAIL  5 lines that no entry names');
  // A change that was too slow in those seconds has it too.
  const slow = lines([], { changes: [change('mount', true, 9, 3)] });
  assert.deepEqual(beside(slow, 'FAIL  mount on settled after 3.0 s').map(line => line.trim()), ['video packets lost: 12 at 10 s, 3 at 11 s']);
  // A run without a failure names the seconds; a run without a lost packet has no such line.
  assert.deepEqual(lines([]), ['HOST  none in the recording: no known failure with "host" applies', 'LOST  video packets: 12 at 10 s, 3 at 11 s', 'PASS']);
  assert.doesNotMatch(lines([12], { lost: false }).join('\n'), /LOST|packets lost/);
});

// takeover with a clock of its own, in ms from the start of the second tab's
// load: the tab has the page's first bytes at loadMs; the page makes its
// signaling socket at madeMs (null: never), the browser sends the request
// 2 ms later, and the Mini accepts it at socketMs (null: never; a number
// under 0: the Mini answers with HTTP 503 at its absolute value); the tab's
// first frame comes at frameMs (null: never). The first tab shows "Use Here"
// shownMs after that frame (null: never) and has its first frame backMs
// after the click (null: never). The first tab's page makes a signaling
// socket 30 ms after the click, which the Mini accepts backSocketMs after
// the click (null: never), and it had one before the takeover.
function takeoverWithClock({ loadMs = 400, madeMs = 450, socketMs = 500, frameMs = 1300, shownMs = 50, backMs = 900, backSocketMs = 80 } = {}) {
  const start = 7_000_000;
  let t = start;
  let back = 'the browser decoded 12 frames in the last second, and 25 make it work';
  const did = [];
  const signaling = signalingSockets();
  // The browser's events up to now reach the tool, each once.
  const told = new Set();
  const tell = (name, params, ms) => {
    if (ms === null || t < start + ms || told.has(name + params.requestId)) return;
    told.add(name + params.requestId);
    signaling.event(name, params, start + ms);
  };
  const sockets = () => {
    tell('Network.webSocketCreated', { requestId: 'other', url: 'ws://mini.test/other' }, 100);
    tell('Network.webSocketCreated', { requestId: '7', url: 'ws://mini.test/webrtc/signaling/client' }, madeMs);
    tell('Network.webSocketWillSendHandshakeRequest', { requestId: '7' }, madeMs === null ? null : madeMs + 2);
    tell('Network.webSocketHandshakeResponseReceived', { requestId: '7', response: { status: socketMs < 0 ? 503 : 101 } },
      socketMs === null ? null : Math.abs(socketMs));
    return signaling.sockets();
  };
  // A page's first frame at `at`, or none: the wait ends at the frame or at its limit.
  const frame = at => async timeoutMs => {
    const end = t + timeoutMs;
    const came = at() !== null && at() <= end;
    t = Math.max(t, came ? at() : end);
    return came ? at() : null;
  };
  let clickedAt = null;
  let secondHasVideo = false;
  // What the tabs make after the session is back: signaling sockets of the second tab, and peer connections of each.
  const later = { sockets: 0, firstPeers: 0, secondPeers: 0, firstSilent: false };
  const takeover = takeoverSwitch({
    openTab: async () => {
      did.push('open the second tab');
      return {
        load: async () => { t += loadMs; did.push('load'); },
        sockets: () => [...sockets(), ...Array.from({ length: later.sockets }, () => ({ createdAt: t, openAt: t }))],
        peers: async () => 1 + later.secondPeers,
        firstFrame: async timeoutMs => {
          const at = await frame(() => (frameMs === null ? null : start + frameMs))(timeoutMs);
          secondHasVideo = at !== null;
          return at;
        },
        close: async () => { did.push('close the second tab'); },
      };
    },
    first: {
      watch: async () => { did.push('watch the first tab'); },
      peers: async () => (later.firstSilent ? null : 2 + later.firstPeers),
      endWatch: async () => { did.push('end the watch'); },
      sockets: async () => {
        const first = signalingSockets();
        first.event('Network.webSocketCreated', { requestId: '1', url: 'ws://mini.test/webrtc/signaling/client' }, start - 60_000);
        first.event('Network.webSocketHandshakeResponseReceived', { requestId: '1', response: { status: 101 } }, start - 59_950);
        if (clickedAt !== null) {
          first.event('Network.webSocketCreated', { requestId: '2', url: 'ws://mini.test/webrtc/signaling/client' }, clickedAt + 30);
          first.event('Network.webSocketWillSendHandshakeRequest', { requestId: '2' }, clickedAt + 32);
          if (backSocketMs !== null) first.event('Network.webSocketHandshakeResponseReceived', { requestId: '2', response: { status: 101 } }, clickedAt + backSocketMs);
        }
        return first.sockets();
      },
      useHereShown: async timeoutMs => {
        t += shownMs ?? timeoutMs;
        return shownMs === null ? null : t;
      },
      // The page gets the click 10 ms after the call.
      useHere: async () => { t += 10; clickedAt = t; did.push('click "Use Here"'); return clickedAt; },
      firstFrame: timeoutMs => frame(() => (backMs === null ? null : clickedAt + backMs))(timeoutMs),
      // The first tab keeps its session until the second tab has one.
      has: async () => ({ video: !secondHasVideo, useHere: secondHasVideo && shownMs !== null }),
    },
    sessionBack: async () => back,
    now: () => t,
    wait: async ms => { t += ms; },
  });
  return { takeover, did, start, later, sessionWorks: () => { back = true; } };
}

test('a takeover is the second tab\'s load, its signaling socket, its first frame, "Use Here" in the first tab and that tab\'s first frame, in this order, with the time of each', async () => {
  const { takeover, did, start, later, sessionWorks } = takeoverWithClock();
  assert.equal(await takeover.sample(), null);
  assert.equal(await takeover.isOn(), false);
  await takeover.on();
  // The second tab stays open, as the tab of a user who leaves both open.
  assert.deepEqual(did, ['watch the first tab', 'open the second tab', 'load', 'click "Use Here"']);
  // The button is clicked when the first tab shows it, 50 ms after the second tab's frame; the page gets the click 10 ms later, and the
  // frame after it comes 900 ms after that.
  // The first tab's socket is the one that its page made after the click, not the one of its session before.
  assert.deepEqual(await takeover.sample(), { loadAt: start, socketMadeAt: start + 450, socketRequestAt: start + 452, socketAt: start + 500,
    frameAt: start + 1300, useHereShownAt: start + 1350, useHereAt: start + 1360, backSocketMadeAt: start + 1390,
    backSocketRequestAt: start + 1392, backSocketAt: start + 1440, backAt: start + 2260, socketsMade: 1, backSocketsMade: 1, newConnections: 0 });
  assert.equal(await takeover.isOn(), true);
  // Two tabs that take the session from each other: each new signaling socket and each new peer connection counts.
  later.sockets = 2;
  later.secondPeers = 1;
  later.firstPeers = 1;
  assert.equal((await takeover.sample()).newConnections, 4);
  // The change is finished when the first tab's session works again.
  assert.match(await takeover.works(true), /decoded 12 frames/);
  sessionWorks();
  assert.equal(await takeover.works(true), true);
  await takeover.off();
  assert.deepEqual(did.slice(-2), ['close the second tab', 'end the watch']);
  assert.equal(await takeover.isOn(), false);
  assert.equal(await takeover.sample(), null);
  assert.equal(await takeover.works(false), true);
});

test('a tab that does not answer for its peer connections leaves the new connections unknown, not counted', async () => {
  // At the end of the change: the count of both tabs is taken at the next sample that has it.
  const late = takeoverWithClock();
  late.later.firstSilent = true;
  await late.takeover.on();
  assert.equal((await late.takeover.sample()).newConnections, null);
  late.later.firstSilent = false;
  assert.equal((await late.takeover.sample()).newConnections, 0);
  // In the hold: one sample without the count, then the count again.
  const { takeover, later } = takeoverWithClock();
  await takeover.on();
  assert.equal((await takeover.sample()).newConnections, 0);
  later.firstSilent = true;
  assert.equal((await takeover.sample()).newConnections, null);
  later.firstSilent = false;
  assert.equal((await takeover.sample()).newConnections, 0);
});

test('a second tab whose signaling socket does not open is an error of its own, with what the browser did and what the first tab has, and the tool does not try again', async () => {
  // The browser sent the request and no answer came.
  const silent = takeoverWithClock({ socketMs: null });
  await assert.rejects(silent.takeover.on(), error => {
    assert.equal(error.message, `the second tab's signaling socket did not open in ${SOCKET_MS / 1000} s: the page made the socket after 450 ms, ` +
      'the browser sent its request after 452 ms, no answer came; the first tab still has its session');
    return true;
  });
  // The time that came is in the recording, and the tab stays for the cleanup, which closes it.
  assert.deepEqual(await silent.takeover.sample(), { loadAt: silent.start, socketMadeAt: null, socketRequestAt: null, socketAt: null, frameAt: null,
    useHereShownAt: null, useHereAt: null, backSocketMadeAt: null, backSocketRequestAt: null, backSocketAt: null, backAt: null,
    socketsMade: 1, backSocketsMade: 0, newConnections: null });
  assert.equal(await silent.takeover.isOn(), false);
  assert.deepEqual(silent.did, ['watch the first tab', 'open the second tab', 'load']);
  await silent.takeover.off();
  assert.deepEqual(silent.did.slice(3), ['close the second tab', 'end the watch']);
  // The page made no socket; the Mini refused the socket.
  await assert.rejects(takeoverWithClock({ madeMs: null, socketMs: null }).takeover.on(),
    /did not open in 10 s: the page made no signaling socket; the first tab still has its session$/);
  await assert.rejects(takeoverWithClock({ socketMs: -480 }).takeover.on(),
    /did not open in 10 s: .*the browser sent its request after 452 ms, the Mini answered with HTTP 503; the first tab still has its session$/);
});

test('a takeover that stops after the socket names the step that did not come, and is not a socket that did not open', async () => {
  const noFrame = takeoverWithClock({ frameMs: null });
  await assert.rejects(noFrame.takeover.on(),
    new RegExp(`^Error: the second tab has no video ${SESSION_MS / 1000} s after its load began; its signaling socket opened after 500 ms$`));
  assert.equal((await noFrame.takeover.sample()).socketAt, noFrame.start + 500);
  const noButton = takeoverWithClock({ shownMs: null });
  await assert.rejects(noButton.takeover.on(),
    new RegExp(`^Error: the first tab does not show "Use Here" ${USE_HERE_SHOWN_MS / 1000} s after the second tab had video$`));
  assert.equal(noButton.did.includes('click "Use Here"'), false);
  // The way back: with a socket that opened, and with one that did not.
  const noReturn = takeoverWithClock({ backMs: null });
  await assert.rejects(noReturn.takeover.on(),
    new RegExp(`^Error: the first tab has no video ${SESSION_MS / 1000} s after "Use Here": its signaling socket opened after 80 ms$`));
  assert.equal(await noReturn.takeover.isOn(), false);
  await assert.rejects(takeoverWithClock({ backMs: null, backSocketMs: null }).takeover.on(),
    /the first tab has no video 20 s after "Use Here": the page made the socket after 30 ms, the browser sent its request after 32 ms, no answer came$/);
  // A slow takeover is no error: the rules judge its times.
  const slow = takeoverWithClock({ socketMs: 6000, frameMs: 9000, backMs: 7000 });
  await slow.takeover.on();
  const { loadAt, socketAt, frameAt, useHereAt, backAt } = await slow.takeover.sample();
  assert.deepEqual([socketAt - loadAt, frameAt - loadAt, backAt - useHereAt], [6000, 9000, 7000]);
});

test('the signaling sockets of a tab are those of the Mini\'s signaling path, each with what the browser did with it', () => {
  const signaling = signalingSockets();
  signaling.event('Network.webSocketCreated', { requestId: '1', url: 'ws://192.168.1.100/webrtc/signaling/client' }, 100);
  signaling.event('Network.webSocketCreated', { requestId: '2', url: 'wss://example.test/live' }, 110);
  signaling.event('Network.webSocketWillSendHandshakeRequest', { requestId: '1' }, 120);
  signaling.event('Network.webSocketWillSendHandshakeRequest', { requestId: '2' }, 125);
  signaling.event('Network.webSocketFrameError', { requestId: '1', errorMessage: 'Connection closed before receiving a handshake response' }, 400);
  signaling.event('Network.webSocketClosed', { requestId: '1' }, 401);
  // The page's second socket, after the first failed.
  signaling.event('Network.webSocketCreated', { requestId: '3', url: 'ws://192.168.1.100/webrtc/signaling/client' }, 900);
  signaling.event('Network.webSocketWillSendHandshakeRequest', { requestId: '3' }, 901);
  signaling.event('Network.webSocketHandshakeResponseReceived', { requestId: '3', response: { status: 101 } }, 950);
  assert.deepEqual(signaling.sockets(), [
    { createdAt: 100, requestAt: 120, openAt: null, answer: null, closedAt: 401, error: 'Connection closed before receiving a handshake response' },
    { createdAt: 900, requestAt: 901, openAt: 950, answer: null, closedAt: null, error: null }]);
});

test('a page\'s first frame is the first report of its open connection with a decoded frame, and null when none comes in time', async () => {
  const before = globalThis.window;
  let frames = 0;
  let state = 'connected';
  globalThis.window = { __kvmTestHooks: { _getPeerConnection: () => ({
    connectionState: state,
    getStats: async () => new Map([['a', { type: 'inbound-rtp', kind: 'audio', framesDecoded: 99 }], ['v', { type: 'inbound-rtp', kind: 'video', framesDecoded: frames }]]),
  }) } };
  try {
    setTimeout(() => { frames = 1; }, 60);
    const began = Date.now();
    const at = await firstFrame(2000);
    assert.ok(at - began >= 55 && at - began < 500, `${at - began} ms`);
    // The connection of the session that was taken over is closed: its frames do not count.
    state = 'closed';
    assert.equal(await firstFrame(100), null);
    globalThis.window = {};
    assert.equal(await firstFrame(50), null);
  } finally {
    globalThis.window = before;
  }
});

// A walk's seconds around a takeover that begins at second `at`: the second
// tab has the session for 3 s, in which the first tab's page has no
// connection. Then the first tab's new session counts its frames, its
// freezes and its sound from zero, and its sound is quiet for 2 s. The
// takeover's times are in ms from its start, null for one that does not
// come.
function takenOver(at, state, { socket = 500, frame = 1300, click = 2400, back = 3300 } = {}) {
  const rows = recording(40, state);
  let frames = 0;
  let energy = 0;
  rows.forEach((row, second) => {
    const away = second > at && second <= at + 3;
    const fresh = second - at - 4;   // the seconds of the new session before this one
    frames = fresh === 0 ? 20 : frames + 30;
    energy = fresh === 0 ? 0.0002 : energy + (fresh === 1 ? 0.0002 : 0.0225);
    const played = fresh >= 0 ? fresh + 1 : second;
    row.browser = away ? null : { connectionState: 'connected',
      video: { framesDecoded: frames, frameHeight: 1080, totalFreezesDuration: fresh >= 0 ? 0.3 : 0, timestamp: row.t } };
    const start = T0 + at * 1000;
    const came = ms => (ms !== null && row.t >= start + ms ? start + ms : null);
    row.loads = {
      audio: away ? null : { totalAudioEnergy: energy, totalSamplesDuration: played, concealedSamples: fresh >= 0 ? 4800 : 0, packetsReceived: played * 50 },
      ...(row.state.takeover ? { takeover: { loadAt: start, socketAt: came(socket), frameAt: came(frame), useHereAt: came(click), backAt: came(back) } } : {}),
    };
  });
  return rows;
}

test('a takeover has each of its three times once, in the first sample that has its end, and a later takeover has its own', () => {
  const limits = rules(...['takeoverMs', 'signalingOpenMs', 'useHereMs'].map(measure => ({ while: { takeover: true }, measure, max: 0 })));
  const state = second => ({ video: true, audio: true, takeover: (second >= 5 && second < 15) || second >= 20 });
  const rows = [...takenOver(5, state).slice(0, 18), ...takenOver(20, state, { socket: 700, frame: 4100, click: 4200, back: 5000 }).slice(18)];
  assert.deepEqual(judge(rows, limits).failures.map(f => [(f.t - T0) / 1000, f.measure, f.value]), [
    [6, 'signalingOpenMs', 500], [7, 'takeoverMs', 1300], [9, 'useHereMs', 900],
    [21, 'signalingOpenMs', 700], [25, 'takeoverMs', 4100], [25, 'useHereMs', 800]]);
  // A takeover that ended at its socket has no time.
  const stopped = takenOver(5, state, { socket: null, frame: null, click: null, back: null });
  assert.deepEqual(judge(stopped.slice(0, 18), limits).failures, []);
});

test('on both boards the takeover pauses the checks on video and the sound until the first tab\'s session is back', () => {
  const files = board => readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
  const ruleOf = board => files(board).changeTakesAtMost.find(line => line.change.takeover === true);
  assert.deepEqual(ruleOf('wifi').pauses, ruleOf('ethernet').pauses);
  const state = second => ({ video: true, audio: true, takeover: second >= 10 && second < 24 });
  const failed = (board, rows) => [...new Set(judge(rows, files(board)).failures.map(f => f.measure ?? `${Object.keys(f.change.change)[0]} took ${f.value}`))].sort();
  for (const board of ['ethernet', 'wifi']) {
    // The takeover begins at 10 s, and the first tab's session works again 6 s later.
    const during = [change('takeover', true, 10, 6, { state: { video: true, audio: true } }), ...takenOver(10, state)];
    assert.deepEqual(failed(board, during), [], board);
    // The same seconds without the change are a session that stops: every paused check fails.
    // The measures of the video stream compare two reports of one session, which this recording does not name,
    // and it has no frequency of the sound.
    const stream = ['keyframesPerSecond', 'browserFramesDropped', 'keyframeRequests', 'framesDecodedShare', 'audioToneHz'];
    assert.deepEqual(stream.filter(measure => !ruleOf(board).pauses.includes(measure)), [], board);
    assert.deepEqual(failed(board, takenOver(10, { video: true, audio: true })), ruleOf(board).pauses.filter(measure => !stream.includes(measure)).sort(), board);
  }
});

// A session with one key every 1.5 s from second 5 on: seconds 5, 6, 8, 9, 11, ... have a key and seconds 7, 10, 13, ... have none.
// lateKeys: the seconds whose key the Mini did not answer within the key's period.
// hid with ten keys per second: each of 20 ms, or of the time that `times`
// gives its second, null for a key without an answer. state(second) is the
// state of each second.
function hidKeyRows(seconds, times = {}, state = () => ({ hid: true })) {
  return recording(seconds, state).map((row, second) => ({ ...row,
    loads: row.state.hid ? { hid: { keys: Array.from({ length: 10 }, (_, i) => [0x04 + i, row.t - 1000 + i * 100, second in times ? times[second] : 20]) } } : {} }));
}

test('a 95th percentile has a value once its rule has applied for its whole window, and its window starts again when the rule changes', () => {
  const P95 = { while: { hid: true }, measure: 'keyLatencyP95Ms', windowSeconds: 5, max: 100 };
  const failed = (rows, criteria) => judge(rows, criteria).failures.map(f => [f.measure, f.t - T0, f.value]);
  // Slow keys in the first two seconds: the first window, of seconds 0 to 4, has 20 of 50 keys at 300 ms.
  assert.deepEqual(failed(hidKeyRows(10, { 0: 300, 1: 300 }), rules(P95)), [['keyLatencyP95Ms', 4000, 300]]);
  // Three slow keys of 50 in a window are over the 95th percentile, two are not; a key without an answer is 3 s.
  assert.deepEqual(failed(hidKeyRows(12, { 7: 300 }).map((row, second) => (second === 7 ? { ...row, loads: { hid: { keys: row.loads.hid.keys.map((key, i) => (i < 3 ? key : [...key.slice(0, 2), 20])) } } } : row)), rules(P95)),
    [['keyLatencyP95Ms', 7000, 300]]);
  assert.deepEqual(failed(hidKeyRows(12, { 7: null }).map((row, second) => (second === 7 ? { ...row, loads: { hid: { keys: row.loads.hid.keys.map((key, i) => (i < 2 ? key : [...key.slice(0, 2), 20])) } } } : row)), rules(P95)), []);
  // A loose rule until second 10, and from then on, with a mount, a strict one: the keys of 300 ms
  // at 6 to 9 are not in the strict rule's window, which has a value from second 14 on.
  const state = second => ({ hid: true, mount: second >= 10 });
  const criteria = rules({ ...P95, max: 1000 }, { ...P95, while: { hid: true, mount: true } });
  assert.deepEqual(failed(hidKeyRows(20, { 6: 300, 7: 300, 8: 300, 9: 300 }, state), criteria), []);
  assert.deepEqual(failed(hidKeyRows(20, { 12: 300 }, state), criteria), [['keyLatencyP95Ms', 14_000, 300]]);
  // A sample in which a change pauses the measure adds no key to the window:
  // the mount's switch is done at 1 s, and a key's answer is judged up to
  // 4.1 s after that.
  const paused = { everySecond: [P95], changeTakesAtMost: [{ change: { mount: true }, max: 5, pauses: ['keyLatencyP95Ms'] }] };
  const during = [change('mount', true, 0, 1), ...hidKeyRows(12, { 3: 300, 4: 300, 5: 300 })];
  assert.deepEqual(failed(during, paused), []);
  assert.deepEqual(failed(during.slice(1), paused), [['keyLatencyP95Ms', 4000, 300]]);
});

test('a 95th percentile has a value only after its whole window of samples in a row without a pause', () => {
  const P95 = { while: { hid: true }, measure: 'keyLatencyP95Ms', windowSeconds: 5, max: 100 };
  const failed = (rows, criteria) => judge(rows, criteria).failures.map(f => [f.measure, f.t - T0, f.value]);
  const paused = { everySecond: [P95], changeTakesAtMost: [{ change: { mount: true }, max: 5, pauses: ['keyLatencyP95Ms'] }] };
  // The mount pauses the measure up to second 5. In second 6, the first
  // after the pause, `slow` of the ten keys take 300 ms.
  const rows = slow => [change('mount', true, 0, 1), ...hidKeyRows(14, { 6: 300 }).map((row, second) => (second === 6 ? { ...row, loads: { hid: { keys: row.loads.hid.keys.map((key, i) => (i < slow ? key : [...key.slice(0, 2), 20])) } } } : row))];
  // The first value is at second 10, of seconds 6 to 10: two slow keys of
  // 50 are not over the 95th percentile, and three are.
  assert.deepEqual(failed(rows(2), paused), []);
  assert.deepEqual(failed(rows(3), paused), [['keyLatencyP95Ms', 10_000, 300]]);
});

test('only a rule of a 95th percentile has "windowSeconds", and it has it', () => {
  const check = line => () => validateCriteria(rules(line), ['hid']);
  check({ while: { hid: true }, measure: 'keyLatencyP95Ms', windowSeconds: 5, max: 100 })();
  assert.throws(check({ while: { hid: true }, measure: 'keyLatencyP95Ms', max: 100 }), /"windowSeconds" belongs to a rule of keyLatencyP95Ms or terminalLineP95Ms/);
  assert.throws(check({ while: { hid: true }, measure: 'keyLatencyMaxMs', windowSeconds: 5, max: 500 }), /"windowSeconds" belongs/);
  assert.throws(check({ while: { hid: true }, measure: 'keyLatencyP95Ms', windowSeconds: 0.5, max: 100 }), /whole number of seconds/);
});

test('the report\'s line of a breach of a key latency has the keys of its second or its window beside it', () => {
  const criteria = rules({ while: { hid: true }, measure: 'keyLatencyP95Ms', windowSeconds: 5, max: 100 },
    { while: { hid: true }, measure: 'keyLatencyMaxMs', max: 500 });
  const rows = hidKeyRows(8, { 6: null });
  assert.deepEqual(report(judge(rows, criteria), T0).split('\n').filter(line => line.startsWith('FAIL  key')), [
    'FAIL  keyLatencyMaxMs is 3000 for more than 0 s (10 keys, 10 over 500 ms, 10 without an answer within 3 s, slowest 3000 ms)',
    'FAIL  keyLatencyP95Ms is 3000 for more than 0 s (the last 5 s: 50 keys, 10 over 100 ms, 10 without an answer within 3 s, slowest 3000 ms)',
  ]);
});

// Slow keys from second 5 on, one in two seconds of three: each with 12 ms,
// or the time that `times` gives its second, null for a key without an
// answer.
function slowKeyRows(times = {}) {
  return recording(30, second => ({ video: true, slowKeys: second >= 5 })).map((row, second) => {
    if (second < 5) return row;
    const key = (second - 5) % 3 !== 2;
    return { ...row, loads: { slowKeys: { keys: key ? [[0x04, row.t - 100, second in times ? times[second] : 12]] : [] } } };
  });
}

test('on both boards the key latency rules judge the slow keys: a key over 100 ms fails once a window is full, and a key without an answer fails at once', () => {
  for (const board of ['ethernet', 'wifi']) {
    const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
    const failed = rows => judge(rows, criteria).failures.map(f => [f.measure, (f.t - T0) / 1000, f.value]);
    assert.deepEqual(failed(slowKeyRows()), [], board);
    // A window of 5 s has 3 or 4 slow keys: its 95th percentile is the slowest.
    assert.deepEqual(failed(slowKeyRows({ 12: 150 })), [['keyLatencyP95Ms', 12, 150]], board);
    // A key without an answer counts as 3 s in both.
    assert.deepEqual(failed(slowKeyRows({ 12: null })), [['keyLatencyMaxMs', 12, 3000], ['keyLatencyP95Ms', 12, 3000]], board);
    // The rules for the slow keys have the limits of the rules for hid's keys.
    for (const measure of ['keyLatencyP95Ms', 'keyLatencyMaxMs']) {
      const rule = name => criteria.everySecond.find(line => line.measure === measure && line.while[name] === true);
      assert.deepEqual({ ...rule('slowKeys'), while: null }, { ...rule('hid'), while: null }, `${board} ${measure}`);
    }
    // The first keys are inside the change, which pauses both rules until
    // the answer of a key sent before its end would be judged.
    const during = [change('slowKeys', true, 5, 1, { state: { video: true } }), ...slowKeyRows({ 6: null, 9: 150 })];
    assert.deepEqual(failed(during), [], board);
  }
});

test('the build has the bench file\'s board, setup code and cloud key URL, and USB debugging on', () => {
  const ethernet = { JETKVM_SKU: 'jetkvm-mini-ethernet', JETKVM_MOCK_CLOUD_JWKS_URL: 'http://192.168.1.102:8791/oidc/jwks' };
  assert.deepEqual(benchBuildArguments(ethernet, false),
    { BOARD: 'ethernet', WIFI_SETUP_LOG_CODE: '0', MOCK_CLOUD_JWKS_URL: 'https://192.168.1.102:8791/oidc/jwks', USB_DEBUGGING: '1' });
  // --plain-cloud: the keys over http, as the bench file has them.
  assert.equal(benchBuildArguments(ethernet, true).MOCK_CLOUD_JWKS_URL, 'http://192.168.1.102:8791/oidc/jwks');
  // --usb-debugging-off: the image starts with USB debugging off, as a release does.
  assert.equal(benchBuildArguments(ethernet, false, false).USB_DEBUGGING, '0');
  // A bench with a Wi-Fi network is set up over BLE with the fixed code; a bench without a cloud builds without one.
  assert.deepEqual(benchBuildArguments({ JETKVM_SKU: 'jetkvm-mini-wireless', JETKVM_WIFI_SSID: 'bench' }, false),
    { BOARD: 'wifi', WIFI_SETUP_LOG_CODE: '1', MOCK_CLOUD_JWKS_URL: '', USB_DEBUGGING: '1' });
  assert.throws(() => benchBuildArguments({ JETKVM_SKU: 'jetkvm' }, false), /JETKVM_SKU "jetkvm"/);
});

test('the build does not take the files of its output directory for changes of the checkout', t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-version-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const git = (...args) => spawnSync('git', ['-C', root, '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false', '-c', 'tag.gpgsign=false', ...args], { encoding: 'utf8' });
  fs.writeFileSync(path.join(root, 'main.c'), 'int main;\n');
  git('init', '-q');
  git('add', 'main.c');
  git('commit', '-q', '-m', 'first');
  git('tag', 'release/0.5.9');
  git('tag', 'release/0.5.11');
  const commit = `0.5.11-g${git('rev-parse', 'HEAD').stdout.slice(0, 5)}`;
  assert.match(commit, /^0\.5\.11-g[0-9a-f]{5}$/);
  // An earlier run's files in --out=results: the version of the clean checkout.
  fs.mkdirSync(path.join(root, 'results', 'walk'), { recursive: true });
  fs.writeFileSync(path.join(root, 'results', 'jetkvm-mini.bin'), 'image');
  fs.writeFileSync(path.join(root, 'results', 'walk', 'recording.json'), '{}');
  assert.equal(benchVersion(root, path.join(root, 'results')), commit);
  assert.equal(outputInCheckout(root, path.join(root, 'results', 'walk')), 'results/walk');
  // The same files are changes for a run that writes to another place, and a changed source file is one for every run.
  assert.match(benchVersion(root, path.join(root, 'other')), /^0\.5\.11-e[0-9a-z]{5}$/);
  assert.match(benchVersion(root, path.join(os.tmpdir(), 'elsewhere')), /^0\.5\.11-e[0-9a-z]{5}$/);
  assert.equal(outputInCheckout(root, path.join(os.tmpdir(), 'elsewhere')), undefined);
  assert.equal(outputInCheckout(root, root), undefined);
  fs.writeFileSync(path.join(root, 'main.c'), 'int main = 1;\n');
  assert.match(benchVersion(root, path.join(root, 'results')), /^0\.5\.11-e[0-9a-z]{5}$/);
});

test('the build runs on the bench file\'s build host, and on this machine\'s Docker for a bench file without one', t => {
  // A checkout with build.mjs and three bench files. docker is a script that
  // writes down its arguments, knows no builder, and puts an image where the
  // build's output goes.
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-build-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const file = (name, text) => {
    fs.mkdirSync(path.dirname(path.join(root, name)), { recursive: true });
    fs.writeFileSync(path.join(root, name), text);
  };
  file('tools/combined_load/runner/build.mjs', fs.readFileSync(path.join(here, 'build.mjs')));
  file('Dockerfile', 'FROM scratch\n');
  file('.dockerignore', '.git\n');
  file('.gitignore', '/out\n/bin\n/docker.log\n/home\n');
  const keys = 'JETKVM_MOCK_CLOUD_JWKS_URL=http://192.168.1.102:8790/oidc/jwks\n';
  file('tools/benches/remote.env', `JETKVM_SKU=jetkvm-mini-wireless\nJETKVM_WIFI_SSID="bench"\nJETKVM_BUILD_HOST=user@192.168.1.102\n${keys}`);
  file('tools/benches/service.env', `JETKVM_SKU=jetkvm-mini-ethernet\nJETKVM_BUILD_HOST=tcp://203.0.113.5:1234\n${keys}`);
  // Values in quotes, as the shell of tools/e2e/run.sh takes them.
  file('tools/benches/local.env', `JETKVM_SKU="jetkvm-mini-ethernet"\n${keys.replace(/=(.*)\n/, "='$1'\n")}`);
  file('bin/docker', `#!/bin/sh
printf '%s\\n' "$*" >> "$DOCKER_LOG"
[ "$2" != inspect ] || exit 1
for a; do
  case "$a" in type=local,dest=*) mkdir -p "\${a#type=local,dest=}" && echo image > "\${a#type=local,dest=}/jetkvm-mini.bin" ;; esac
done
`);
  fs.chmodSync(path.join(root, 'bin/docker'), 0o755);
  const git = (...args) => spawnSync('git', ['-C', root, '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false', '-c', 'tag.gpgsign=false', ...args], { encoding: 'utf8' });
  git('init', '-q');
  git('add', '.');
  git('commit', '-q', '-m', 'first');
  git('tag', 'release/0.5.11');
  const log = path.join(root, 'docker.log');
  const build = (...args) => {
    fs.rmSync(log, { force: true });
    const result = spawnSync(process.execPath, [path.join(root, 'tools/combined_load/runner/build.mjs'), ...args],
      { encoding: 'utf8', env: { ...process.env, HOME: path.join(root, 'home'), PATH: `${path.join(root, 'bin')}:${process.env.PATH}`, DOCKER_LOG: log } });
    return { ...result, calls: fs.existsSync(log) ? fs.readFileSync(log, 'utf8').trim().split('\n') : [] };
  };
  const out = path.join(root, 'out');
  const argument = (call, name) => call.match(new RegExp(`--build-arg ${name}=(\\S*)`))?.[1];

  // A bench file with a build host: the builder of that machine, made at the
  // first build, builds, and its cache is cut down afterwards.
  const remote = build('remote', out);
  assert.equal(remote.status, 0, remote.stderr);
  assert.equal(remote.stdout, `${path.join(out, 'jetkvm-mini.bin')}\n`);
  assert.deepEqual(remote.calls.map(call => call.split(' ').slice(0, 2).join(' ')), ['buildx inspect', 'buildx create', 'buildx build', 'buildx prune']);
  assert.match(remote.calls[1], /--name jetkvm-user-192-168-1-102 --driver docker-container ssh:\/\/user@192\.168\.1\.102$/);
  assert.match(remote.calls[2], /^buildx build --builder jetkvm-user-192-168-1-102 /);
  assert.match(remote.calls[3], /--builder jetkvm-user-192-168-1-102 /);
  assert.match(argument(remote.calls[2], 'VERSION'), /^0\.5\.11-g[0-9a-f]{5}$/);
  assert.deepEqual(['BOARD', 'WIFI_SETUP_LOG_CODE', 'MOCK_CLOUD_JWKS_URL'].map(name => argument(remote.calls[2], name)),
    ['wifi', '1', 'https://192.168.1.102:8790/oidc/jwks']);
  assert.ok(remote.calls[2].endsWith(` --output type=local,dest=${out} ${root}`), remote.calls[2]);

  // A build service at tcp://host:port: without the client files no build,
  // and the message names their folder.
  const certs = path.join(root, 'home', '.docker', 'buildx', 'certs', 'jetkvm-tcp-203-0-113-5-1234');
  fs.mkdirSync(certs, { recursive: true });
  fs.writeFileSync(path.join(certs, 'ca.pem'), 'ca');
  fs.writeFileSync(path.join(certs, 'client.pem'), 'cert');
  const missing = build('service', out);
  assert.equal(missing.status, 1);
  assert.ok(missing.stderr.includes(certs), missing.stderr);
  assert.deepEqual(missing.calls, []);
  // With them, a builder of buildx's remote driver that reads them, made at
  // the first build, builds, and its cache is cut down afterwards.
  fs.writeFileSync(path.join(certs, 'client-key.pem'), 'key');
  const service = build('service', out);
  assert.equal(service.status, 0, service.stderr);
  assert.deepEqual(service.calls.map(call => call.split(' ').slice(0, 2).join(' ')), ['buildx inspect', 'buildx create', 'buildx build', 'buildx prune']);
  assert.equal(service.calls[1], `buildx create --name jetkvm-tcp-203-0-113-5-1234 --driver remote --driver-opt cacert=${certs}/ca.pem,cert=${certs}/client.pem,key=${certs}/client-key.pem tcp://203.0.113.5:1234`);
  assert.match(service.calls[2], /^buildx build --builder jetkvm-tcp-203-0-113-5-1234 /);
  assert.match(service.calls[3], /--builder jetkvm-tcp-203-0-113-5-1234 /);

  // A bench file without one: one call, to this machine's Docker. The e2e
  // suite's old version and its key URL as the bench file has it.
  const local = build('local', out, '--version=0.0.1', '--plain-cloud');
  assert.equal(local.status, 0, local.stderr);
  assert.equal(local.calls.length, 1);
  assert.doesNotMatch(local.calls[0], /--builder/);
  assert.deepEqual(['VERSION', 'BOARD', 'WIFI_SETUP_LOG_CODE', 'MOCK_CLOUD_JWKS_URL'].map(name => argument(local.calls[0], name)),
    ['0.0.1', 'ethernet', '0', 'http://192.168.1.102:8790/oidc/jwks']);

  // No option names the build host.
  const option = build('local', out, '--build-host=user@192.168.1.102');
  assert.equal(option.status, 2);
  assert.deepEqual(option.calls, []);
});

// An MQTT 3.1.1 packet: the type and flags, the body's length, the body.
const mqttPacket = (first, ...parts) => {
  const body = Buffer.concat(parts.map(part => (Buffer.isBuffer(part) ? part : Buffer.from(part))));
  assert.ok(body.length < 128, 'a test packet has one byte of length');
  return Buffer.concat([Buffer.from([first, body.length]), body]);
};
const mqttText = text => Buffer.concat([Buffer.from([text.length >> 8, text.length & 0xff]), Buffer.from(text)]);
// A PUBLISH with QoS 1 and the retain flag, as the Mini sends its states.
const mqttPublish = (topic, id, payload) => mqttPacket(0x33, mqttText(topic), [id >> 8, id & 0xff], payload);
// A client that connects to the broker as the Mini does: over TLS without
// verification, with the Mini's client ID and its last will on `status`.
// acknowledged(count) waits for that many bytes of the broker's answers.
async function mqttMini(port, deviceId) {
  const socket = tls.connect({ port, host: '127.0.0.1', rejectUnauthorized: false });
  let received = 0;
  socket.on('data', chunk => { received += chunk.length; });
  socket.on('error', () => undefined);
  await new Promise((resolve, reject) => socket.once('secureConnect', resolve).once('error', reject));
  const acknowledged = async count => {
    const wanted = received + count;
    await until(() => received >= wanted, 2000, () => `the broker sent ${received} bytes, not ${wanted}`);
  };
  // The flags: a last will with QoS 1 and retain, and a clean session.
  socket.write(mqttPacket(0x10, mqttText('MQTT'), [4, 0x2e, 0, 30], mqttText(`jetkvm-${deviceId}`),
    mqttText(`${BASE_TOPIC}/${deviceId}/status`), mqttText('{"online":false}')));
  await acknowledged(4);
  return {
    socket,
    publish: async (suffix, payload) => {
      socket.write(mqttPublish(`${BASE_TOPIC}/${deviceId}/${suffix}`, 1, payload));
      await acknowledged(4);
    },
  };
}

test('mqtt gives the Mini the tool\'s broker over TLS and puts back the settings that the Mini had', async () => {
  const stored = { enabled: false, broker: 'broker.test', port: 8883, username: 'u', password: '********', base_topic: 'kvm', use_tls: false,
    tls_insecure: false, enable_ha_discovery: false, enable_actions: false, debounce_ms: 500 };
  const { mini, rpc: settings } = fakeMini({ mqtt: stored });
  const rpc = (method, params) => (method === 'getDeviceID' ? 'abc123' : settings(method, params));
  let modeValid;
  const mqtt = mqttSwitch({ rpc, address: '127.0.0.1', stats: async () => ({ modeValid }) });
  assert.equal(await mqtt.isOn(), false);
  await mqtt.off();   // the cleanup before a list: nothing to put back
  assert.deepEqual(mini.mqtt, stored);
  // A sample before the Mini has the broker has no start time, so no measure has a value.
  assert.deepEqual(mqtt.sample(), { since: null, connected: false, statusAt: null, videoAt: null, video: null });

  const called = Date.now();
  await mqtt.on();
  const { port } = mini.mqtt;
  // What the tool sets, and the rest as the Mini had it: the mask keeps the Mini's password.
  assert.deepEqual(mini.mqtt, { ...stored, enabled: true, broker: '127.0.0.1', port, base_topic: BASE_TOPIC, use_tls: true,
    tls_insecure: true, enable_ha_discovery: true });
  assert.equal(await mqtt.isOn(), false, 'on when the Mini has connected, not when the setting is stored');
  assert.ok(mqtt.sample().since >= called);
  // The broker speaks TLS only.
  const plain = net.connect(port, '127.0.0.1');
  plain.on('error', () => undefined);
  plain.write(mqttPacket(0x10, mqttText('MQTT'), [4, 2, 0, 30], mqttText('jetkvm-abc123')));
  await new Promise(resolve => plain.once('close', resolve));
  assert.equal(await mqtt.isOn(), false);

  // Another client is not the Mini, and neither are its messages.
  const other = await mqttMini(port, 'ffff');
  await other.publish('video/state', '{"ready":false}');
  assert.equal(await mqtt.isOn(), false);
  const client = await mqttMini(port, 'abc123');
  assert.equal(await mqtt.isOn(), true);
  assert.match(await mqtt.works(true), /no video\/state message/);
  const before = Date.now();
  await client.publish('video/state', '{"ready":true,"width":1920,"height":1080,"fps":60.0}');
  // A Mini whose short stats have no video mode cannot be compared with its messages.
  assert.match(await mqtt.works(true), /modeValid/);
  modeValid = true;
  assert.equal(await mqtt.works(true), true);
  const sample = mqtt.sample();
  assert.deepEqual(sample.video, { ready: true, width: 1920, height: 1080, fps: 60 });
  assert.ok(sample.connected && sample.videoAt >= before && sample.since <= sample.videoAt);
  assert.equal(sample.statusAt, null);
  await client.publish('status', '{"online":true}');
  const { statusAt } = mqtt.sample();
  assert.ok(statusAt >= sample.videoAt);
  // A payload that is not JSON says nothing that agrees with the Mini.
  await client.publish('video/state', 'ready');
  assert.deepEqual(mqtt.sample().video, {});
  // A Mini that restarts closes nothing. The broker then publishes its last will on the status topic, which is
  // no status message of the Mini.
  client.socket.destroy();
  await until(async () => !(await mqtt.isOn()), 2000, 'the broker keeps a client whose connection is closed');
  assert.equal(mqtt.sample().statusAt, statusAt);

  await mqtt.off();
  assert.deepEqual(mini.mqtt, stored);
  assert.equal(await mqtt.isOn(), false);
  assert.equal(mqtt.sample().since, null);
  await new Promise(resolve => net.connect(port, '127.0.0.1').once('error', resolve));   // the broker is closed
});

// One sample per second with MQTT on: the Mini's mode in its answer, and the
// newest video/state and status messages with their ages in seconds. The
// Mini got the broker `since` seconds before the first sample, and its clock
// started 600 s before it.
const mqttRows = seconds => seconds.map(({ mode = { modeValid: true, width: 1920, height: 1080 }, video = { ready: true, width: 1920, height: 1080, fps: 60 }, age = 1, statusAge, since = 60 }, second) => ({
  t: T0 + second * 1000, state: { video: true, mqtt: true },
  device: mode && { nowMs: 600_000 + second * 1000, ...mode }, browser: null,
  loads: { mqtt: { connected: true, since: T0 - since * 1000, statusAt: video ? T0 + (second - (statusAge ?? age)) * 1000 : null,
    videoAt: video ? T0 + (second - age) * 1000 : null, video, at: T0 + second * 1000 } },
}));
const MQTT_RULES = rules({ while: { mqtt: true }, measure: 'mqttVideoWrong', max: 0, failAfterSeconds: 5 },
  { while: { mqtt: true }, measure: 'mqttSilentSeconds', max: 20 });
const LOST = { modeValid: false, width: 0, height: 0 };
const NO_SIGNAL = { ready: false, error: 'no_signal', width: 0, height: 0, fps: 0 };

test('MQTT may say the old video state for the 5 s that the Mini takes to publish a lost signal, and no longer', () => {
  const agree = Array(3).fill({});
  // The Mini loses its signal; the message that says so comes after 5 samples, and after 6.
  const lostFor = seconds => [...agree, ...Array(seconds).fill({ mode: LOST }), ...Array(3).fill({ mode: LOST, video: NO_SIGNAL })];
  assert.equal(judge(mqttRows(lostFor(5)), MQTT_RULES).result, 'pass');
  const late = judge(mqttRows(lostFor(6)), MQTT_RULES);
  assert.deepEqual(late.failures.map(failure => [failure.measure, failure.value, failure.t]), [['mqttVideoWrong', 1, T0 + 8000]]);
  // The other direction: the signal is back, and MQTT still says "no signal".
  const back = [...Array(3).fill({ mode: LOST, video: NO_SIGNAL }), ...Array(6).fill({ video: NO_SIGNAL })];
  assert.deepEqual(judge(mqttRows(back), MQTT_RULES).failures.map(failure => failure.measure), ['mqttVideoWrong']);
});

test('MQTT has the Mini\'s picture size: another width or height is wrong, and fps is not compared', () => {
  const size = (width, height, fps = 60) => Array(6).fill({ mode: { modeValid: true, width: 2560, height: 1440 }, video: { ready: true, width, height, fps } });
  assert.equal(judge(mqttRows(size(2560, 1440, 29.9)), MQTT_RULES).result, 'pass');
  assert.equal(judge(mqttRows(size(1920, 1440)), MQTT_RULES).result, 'fail');
  assert.equal(judge(mqttRows(size(2560, 1080)), MQTT_RULES).result, 'fail');
  // A message without "ready" says neither that the Mini has a signal nor that it has none.
  assert.equal(judge(mqttRows(Array(6).fill({ video: {} })), MQTT_RULES).result, 'fail');
  assert.equal(judge(mqttRows(Array(6).fill({ mode: LOST, video: {} })), MQTT_RULES).result, 'fail');
  // A message from before the Mini's last start is not compared: the Mini is 3 s old and has a mode, and the
  // broker's message of 10 s ago says "no signal". The first message after the start is compared again.
  const young = (rows, from) => mqttRows(rows).map((row, second) => ({ ...row, device: { ...row.device, nowMs: (from + second) * 1000 } }));
  assert.equal(judge(young(Array(8).fill({ video: NO_SIGNAL, age: 10 }), 3), MQTT_RULES).result, 'pass');
  assert.equal(judge(young(Array(8).fill({ video: NO_SIGNAL, age: 1 }), 3), MQTT_RULES).result, 'fail');
});

test('MQTT keeps the last video state while the Mini switches its video source, which is not wrong', () => {
  const switching = { mode: { ...LOST, guestVideo: { source: 'switching_to_guest' } } };
  const guest = { modeValid: true, width: 2560, height: 1440, guestVideo: { source: 'guest' } };
  // Eight seconds of a switch, then the sender's mode, which MQTT has two samples later.
  const rows = [...Array(2).fill({}), ...Array(8).fill(switching), ...Array(2).fill({ mode: guest }),
    ...Array(3).fill({ mode: guest, video: { ready: true, width: 2560, height: 1440, fps: 30 } })];
  assert.equal(judge(mqttRows(rows), MQTT_RULES).result, 'pass');
  // Without a mode and without a switch, the same seconds are a lost signal that MQTT does not report.
  const lost = rows.map(row => (row === switching ? { mode: { ...LOST, guestVideo: { source: 'physical' } } } : row));
  assert.deepEqual(judge(mqttRows(lost), MQTT_RULES).failures.map(failure => failure.measure), ['mqttVideoWrong']);
  // A sample without the Mini's answer has no value, and does not end the count of the seconds around it.
  const unanswered = [...Array(3).fill({ mode: LOST }), { mode: null }, ...Array(3).fill({ mode: LOST })];
  assert.equal(judge(mqttRows(unanswered), MQTT_RULES).result, 'fail');
});

const FIRST_STATUS = limit => rules({ while: { mqtt: true }, measure: 'mqttFirstStatusSeconds', max: limit });
// The Mini's answers of mqttRows with another clock: the Mini is `age` seconds old in the first sample.
const aged = (rows, age) => rows.map((row, second) => ({ ...row, device: row.device && { ...row.device, nowMs: (age + second) * 1000 } }));

test('MQTT is silent for at most 20 s, counted from the newest status message', () => {
  // The video topic alone may be quiet for two rounds, as when a round falls into a switch of the video source.
  assert.equal(judge(mqttRows([{ age: 29, statusAge: 14 }, { age: 30, statusAge: 0 }]), MQTT_RULES).result, 'pass');
  assert.equal(judge(mqttRows([{ age: 1, statusAge: 21 }]), MQTT_RULES).result, 'fail');
  const silent = judge(mqttRows([{ age: 16 }, { age: 20 }, { age: 21 }, { age: 1 }]), MQTT_RULES);
  assert.deepEqual(silent.failures.map(failure => [failure.measure, failure.value, failure.t]), [['mqttSilentSeconds', 21, T0 + 2000]]);
  // A status message from before the Mini got the broker, or from before the Mini's last start, is no message
  // of this connection: no silence is counted from it, however old it is.
  assert.equal(judge(mqttRows(Array(4).fill({ statusAge: 40, since: 5 })), MQTT_RULES).result, 'pass');
  assert.equal(judge(aged(mqttRows(Array(4).fill({ statusAge: 40 })), 22), MQTT_RULES).result, 'pass');
  // A sample without the Mini's answer counts on.
  const unanswered = judge(mqttRows([{ age: 19 }, { age: 20, mode: null }, { age: 21, mode: null }]), MQTT_RULES);
  assert.deepEqual(unanswered.failures.map(failure => [failure.measure, failure.value]), [['mqttSilentSeconds', 21]]);
});

test('the Mini\'s first status message on MQTT is timed from when the Mini got the broker and from each start of the Mini', () => {
  const values = (rows, limit) => judge(rows, FIRST_STATUS(limit)).failures.map(failure => [failure.value, failure.t - T0]);
  // The Mini got the broker 2 s before the first sample, and its first status comes 3.5 s after that.
  const on = mqttRows([{ video: null, since: 2 }, { video: null, since: 2 }, { statusAge: 0.5, since: 2 }, { statusAge: 1.5, since: 2 }]);
  assert.deepEqual(values(on, 3.4), [[3.5, 2000]]);
  assert.deepEqual(values(on, 3.5), []);
  // Without a status message the seconds so far are the value, so a limit fails when it has passed.
  const never = mqttRows(Array(4).fill({ video: null, since: 29 }));
  assert.equal(judge(never.slice(0, 2), FIRST_STATUS(30)).result, 'pass');
  assert.deepEqual(values(never, 30), [[31, 2000]]);
  // The sample that has the message has the value, and no sample after it has one, also after a sample
  // without the switch's report.
  const reported = mqttRows(Array(20).fill({}));
  reported[5].loads.mqtt = null;
  assert.deepEqual(values(reported, 0), [[59, 0]]);

  // A Mini that restarted 22 s before the first sample: the broker's status is from before the start for four
  // samples, and the first one of this start comes 26 s after the start.
  const restarted = aged(mqttRows([...Array(4).fill({ statusAge: 40 }), { statusAge: 0 }, { statusAge: 1 }, { statusAge: 21 }]), 22);
  assert.deepEqual(values(restarted, 25.5), [[26, 4000]]);
  assert.deepEqual(values(restarted, 26), []);
  // From that message on the silence counts again.
  assert.deepEqual(judge(restarted, MQTT_RULES).failures.map(failure => [failure.measure, failure.value, failure.t - T0]),
    [['mqttSilentSeconds', 21, 6000]]);

  // A restart in the recording: the Mini does not answer from second 2 to second 4, and its first status of
  // the new start comes at second 3. The answer at second 5, from a Mini that is 4 s old, shows the restart
  // and gives the message its value: 2 s after the start.
  const clocks = [600, 601, null, null, null, 4, 5];
  const restart = mqttRows(clocks.map((clock, second) => ({ since: 1, statusAge: second < 3 ? second + 1 : second - 3, mode: clock === null ? null : undefined })))
    .map((row, second) => ({ ...row, device: row.device && { ...row.device, nowMs: clocks[second] * 1000 } }));
  assert.deepEqual(values(restart, 1.5), [[2, 5000]]);
  assert.deepEqual(values(restart, 2), []);
  // The same when the sample that shows the restart has no report of the switch: the next report has the value.
  restart[5].loads.mqtt = null;
  assert.deepEqual(values(restart, 1.5), [[2, 6000]]);
});
