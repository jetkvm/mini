#!/usr/bin/env node
// Judges a recording against the rules. Reads files only:
//
//   node tools/combined_load/runner/judge.mjs <recording.jsonl> [criteria.jsonc] [known-failures.jsonc]
//
// Without the two files it uses the recording's board's (../ethernet, ../wifi).
//
// A change's switch is done when the variable has its new value and works.
// The change is settled when every second is normal for
// SETTLE_CONFIRM_SECONDS in a row (driver.mjs); a second is not normal when
// a measure breaks its "everySecond" rule and no known failure covers it.
// The settling time runs from the switch call to the start of those seconds.
//
// criteria.jsonc:
//
//   "changeTakesAtMost": [{ "change": { <variable>: <on> }, "max": seconds, "pauses": [m], "pauseSeconds": s }]
//       Limit of a change's settling time. "pauses" are measures not checked
//       until the switch is done, or "pauseSeconds" after it.
//   "everySecond": [{ "while": {...}, "measure": m, "min"|"max": n, "failAfterSeconds": s, "windowSeconds": w }]
//       Checked on every sample (one per second). Fails when broken for more
//       than "failAfterSeconds" in a row, else at once. Of two "while" rules
//       for one measure, the one naming more variables applies; a tie is
//       undecided. P95 measures take "windowSeconds".
//
// "measure" may be a list, and "andOneOf": [<variable>, ...] makes one rule
// per variable with it on; both also work in known failures.
//
// known-failures.jsonc, one breach per entry:
//
//   { "issue": url, "while": {...}, "measure": m, "min"|"max": n, "tolerateSeconds": s, "recoverySeconds": r }
//   { "issue": url, "change": { <variable>: <on> }, "while": {...}, "measure": m, "min"|"max": n, "tolerateSeconds": s, "recoverySeconds": r }
//   { "issue": url, "change": { <variable>: <on> }, "while": {...}, "max": seconds }
//
// First form: tolerated for "tolerateSeconds" in a row while "while" holds,
// and for "recoverySeconds" after. Second form: during that change's switch,
// and "recoverySeconds" after it is done. "min"/"max" bound the tolerated
// value. Third form: the change's settling time. "host" limits an entry to
// one host OS. An entry whose state came up without the breach is a note.
//
// "fails": true marks a known fault: its lines stay FAIL with the issue
// named. A line also covered by an entry without "fails" is known.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { beepDelayMs, levelBetween, middleTone } from './audio.mjs';
import { SETTLE_CAP_SECONDS, settledAt } from './driver.mjs';
import { hostInputAt } from './hid_times.mjs';
import { HOST_NAMES } from './hosts.mjs';
import { settingLines } from './settings.mjs';

// One value per sample; undefined when the sample cannot give one. The extra
// arguments are precomputed per sample by the *At() functions below. `last`
// holds the newest earlier report of each kind, so a count missed by one
// sample lands in the next; a rate uses only the previous sample.
const measures = {
  statsResponsesPerSecond: sample => (sample.device ? 1 : 0),
  // The device's clock goes back only when it restarts.
  deviceRestarts: (sample, previous, lastDevice) =>
    (sample.device && lastDevice ? Number(sample.device.nowMs < lastDevice.nowMs) : undefined),
  // The Mini's encoder, from "videoEncode" in its stats. An encode time is
  // the duration of the encoder call for one frame. The call starts the
  // hardware encoder and waits for its frame-done interrupt, so the time is
  // the hardware's for that frame and the wake-up of the encoder task after
  // it: it follows the video mode and says little about the CPU. encodeMs is
  // its mean, in milliseconds, over the frames since the answer before that
  // are not keyframes. No value in a second without such a frame, as in
  // every second of service video, which the sender encodes.
  encodeMs: (sample, previous, lastDevice, last) => encodeMean(sample.device, last.device, ['pFrames'], 'pEncodeUs'),
  // The same for keyframes; HDMI capture has two a second.
  keyframeEncodeMs: (sample, previous, lastDevice, last) => encodeMean(sample.device, last.device, ['idrFrames'], 'idrEncodeUs'),
  // The longest encoder call the Mini reports, for a frame of either kind:
  // of its current 2-second window and the window before it.
  encodeMaxMs: sample => longestEncodeUs(sample) / 1000 || undefined,
  // The hand-over is the time from the end of a frame's capture to the start
  // of its encoder call. One task encodes one frame at a time, so a frame
  // whose capture ends while the frame before it is in the encoder waits for
  // it; the rest is the wake-up of that task and its preparation of the
  // call, without a copy. The Mini drops a frame that has waited longer than
  // a frame period, and such a frame has no time here. encodeHandoffMs is
  // the mean, in milliseconds, over the frames and keyframes since the
  // answer before.
  encodeHandoffMs: (sample, previous, lastDevice, last) =>
    encodeMean(sample.device, last.device, ['pFrames', 'idrFrames'], 'handoffUs'),
  // The longest hand-over the Mini reports, of the same two windows. No
  // value when the Mini encoded no frame in them.
  encodeHandoffMaxMs: sample => (longestEncodeUs(sample) > 0 ? sample.device.videoEncode.handoffMaxUs / 1000 : undefined),
  framesPerSecond: (sample, previous) => perSecond(sample, previous, 'framesDecoded',
    sample.browser?.video, previous?.browser?.video, 'timestamp'),
  // The longest time between two frames that the tool's page presented since
  // the sample before, in ms: 33 when 30 frames came evenly. The time since
  // the newest frame counts as well, so a second without a frame reports
  // 1000 or more, and more in each second that follows without one. No
  // value before the first frame (longestFrameGap).
  frameGapMs: sample => longestFrameGap(sample.browser?.frames),
  // The time Chrome counted as freezes since the sample before. A freeze is
  // a frame that comes more than about 150 ms late, and it is counted in the
  // second in which that frame arrives. Chrome calls a gap of more than 5 s
  // a pause and counts it apart; both are freezes here.
  freezeMs: (sample, previous, lastDevice, last) => {
    const stalled = video => (video?.totalFreezesDuration === undefined ? undefined
      : video.totalFreezesDuration + (video.totalPausesDuration ?? 0));
    const now = stalled(sample.browser?.video);
    const before = stalled(last.video);
    return now === undefined || before === undefined ? undefined : Math.max(0, Math.round((now - before) * 1000));
  },
  // 1 when the Mini's video source is the sender's service video, 0 when it
  // is HDMI capture or a switch between the two.
  serviceVideo: sample => (sample.device ? Number(sample.device.guestVideo?.source === 'guest') : undefined),
  // The mean quantizer of the frames decoded since the sample before: H.264's
  // measure of how coarse the picture is, 0 (best) to 51.
  videoQuantizer: (sample, previous, lastDevice, last) => {
    const now = sample.browser?.video;
    const before = last.video;
    const frames = now?.framesDecoded - before?.framesDecoded;
    return frames > 0 && now.qpSum >= before.qpSum ? (now.qpSum - before.qpSum) / frames : undefined;
  },
  // 1 when the Mini reports that an upload to the TF card holds video off.
  videoPausedByUpload: sample => (sample.device ? Number(Boolean(sample.device.videoPausedBy?.includes('upload'))) : undefined),
  // The stream of video within one session of the page (browser.peer is the
  // same as in the report before). Keyframes that the browser decoded per
  // second, over the last KEYFRAME_SECONDS (keyframesAt): HDMI capture sends
  // one every half second on the Ethernet Mini and one per second on the
  // Wi-Fi Mini.
  keyframesPerSecond: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share, keyframes) => keyframes,
  // Frames that the browser received and dropped before it showed them.
  browserFramesDropped: (sample, previous, lastDevice, last) => {
    const [now, before] = sameStream(sample, last);
    return increase(now?.framesDropped, before?.framesDropped);
  },
  // The browser's requests to the Mini for a keyframe (PLI), which it sends
  // when it cannot decode what it has.
  keyframeRequests: (sample, previous, lastDevice, last) => {
    const [now, before] = sameStream(sample, last);
    return increase(now?.pliCount, before?.pliCount);
  },
  // 1 when the session's video stream is another one than in the report
  // before, by its SSRC or its id in the browser's reports, else 0.
  videoStreamReplaced: (sample, previous, lastDevice, last) => {
    const now = sample.browser;
    if (!now?.video || !last.video || now.peer === undefined || now.peer !== last.peer) return undefined;
    return Number(now.video.ssrc !== last.video.ssrc || now.video.id !== last.video.id);
  },
  // The share of the frames that the Mini sent which the browser decoded,
  // over the last DECODED_SHARE_SECONDS (decodedShareAt).
  framesDecodedShare: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share) => share,
  // The Mini's counts for service video, each since the Mini's answer
  // before: faults of the service video path, and switches of the video
  // source that failed, and frames from the sender that the Mini dropped
  // (no send slot within a second, or a frame larger than a slot).
  serviceVideoFaults: (sample, previous, lastDevice) => grown(sample.device, lastDevice, device => device.guestVideo?.faults),
  serviceVideoSwitchFailures: (sample, previous, lastDevice) => grown(sample.device, lastDevice, device => device.guestVideo?.switchFailures),
  serviceVideoFramesDropped: (sample, previous, lastDevice) => grown(sample.device, lastDevice, device => device.guestVideo?.framesDropped),
  // The height of the picture in pixels.
  videoHeight: sample => sample.browser?.video?.frameHeight,
  // Video packets that did not reach the browser, since the sample before.
  videoPacketsLost: (sample, previous, lastDevice, last) => increase(sample.browser?.video?.packetsLost, last.video?.packetsLost),
  // Frames that the Mini encoded, or took from the sender, and did not send
  // because the frame was larger than the buffer of its send slot, since the
  // Mini's answer before.
  framesTooBig: (sample, previous, lastDevice) => grown(sample.device, lastDevice, device => device.accessUnitsOversize),
  // Frames that the Mini had and did not send, per second since the Mini's
  // answer before: an answer that follows a missing one counts for the
  // seconds of both. In all: frames whose packets the network stack did not take
  // (accessUnitsStalled). With HDMI capture: captured frames that were not
  // encoded because every send slot still held a frame (accessUnitsNoSlot).
  // With service video the Mini counts there each time a frame asks for a
  // slot, and a frame that waits asks many times and can still be sent, so
  // the count is of the frames that the session dropped
  // (guestVideo.framesDropped: no slot within a second, or a frame larger
  // than a slot).
  framesSkipped: (sample, previous, lastDevice) => {
    const hdmi = device => (device.guestVideo?.source ?? 'physical') === 'physical';
    const stalled = grown(sample.device, lastDevice, device => device.accessUnitsStalled);
    if (stalled === undefined) return undefined;
    const frames = stalled + (grown(sample.device, lastDevice, device => device.guestVideo?.framesDropped) ?? 0) +
      (hdmi(sample.device) && hdmi(lastDevice) ? grown(sample.device, lastDevice, device => device.accessUnitsNoSlot) ?? 0 : 0);
    return frames / secondsBetween(sample.device, lastDevice);
  },
  mountReadBytesPerSecond: (sample, previous) => perSecond(sample, previous, 'bytesRead', mount(sample), mount(previous)),
  // Blocks of 1 MiB of the mounted image that the host read with another
  // SHA-256 than the image has there, since the mount's report before; the
  // host's reader counts them from the mount on (hosts.mjs). A block of the
  // image on the TF card that is wrong at its first read of the run, or at
  // its first read after a restart of the Mini, is an upload that is not on
  // the card as it was sent. A block that the host has read before, and
  // every block of an image from a URL, is a read that went wrong.
  uploadsCorrupt: (sample, previous, lastDevice, last) => sinceMount(sample, last, 'blocksWrongFirst'),
  mountReadsCorrupt: (sample, previous, lastDevice, last) => sinceMount(sample, last, 'blocksWrongLater'),
  // Commands for the mounted medium that the host's kernel counted as
  // answered with an error, since the sample before. Only a Linux host has
  // the count.
  mountReadErrors: (sample, previous, lastDevice, last) => {
    const [name, now] = Object.entries(sample.loads ?? {}).find(([, load]) => load?.bytesRead !== undefined) ?? [];
    return increase(now?.readErrors, last.loads[name]?.readErrors);
  },
  // 1 when the tool's cloud has the Mini's connection, else 0.
  cloudConnected: sample => (sample.loads?.cloud ? Number(sample.loads.cloud.connected) : undefined),
  // 1 when the Mini reports itself running on the tailnet and the page's
  // session is at the Mini's tailnet address, else 0. No value in a second
  // in which the Mini did not answer.
  tailscaleRunning: sample => {
    const load = sample.loads?.tailscale;
    if (!load || load.running === null) return undefined;
    return Number(load.running && load.onTailnet);
  },
  // The time of an adoption in ms, from the browser's request for the
  // Mini's /adopt page to its arrival at the cloud's name page: the Mini
  // serves the page, registers with the cloud and answers, and the page goes
  // on. Each adoption has its value once, in the first sample that has its
  // arrival; no value in any other second.
  adoptMs: (sample, previous, lastDevice, last) => {
    const now = sample.loads?.cloud;
    return typeof now?.namePageAt === 'number' && now.namePageAt !== last.loads.cloud?.namePageAt
      ? now.namePageAt - now.adoptAt : undefined;
  },
  // A takeover of the session by a second tab of the browser, and its way
  // back to the first tab (takeover.mjs). takeoverMs: from the start of the
  // second tab's load of the Mini's page to that tab's first decoded frame
  // of video. signalingOpenMs: from the same start to the Mini's acceptance
  // of that tab's signaling WebSocket. useHereMs: from the click on "Use
  // Here" in the first tab to that tab's first decoded frame. Each takeover
  // has each value once, in the first sample that has its end; no value in
  // any other second.
  takeoverMs: (sample, previous, lastDevice, last) => once(sample.loads?.takeover, last.loads.takeover, 'loadAt', 'frameAt'),
  signalingOpenMs: (sample, previous, lastDevice, last) => once(sample.loads?.takeover, last.loads.takeover, 'loadAt', 'socketAt'),
  useHereMs: (sample, previous, lastDevice, last) => once(sample.loads?.takeover, last.loads.takeover, 'useHereAt', 'backAt'),
  // Signaling sockets and peer connections that either tab made after the
  // session was back in the first tab, since the sample before.
  takeoverNewConnections: (sample, previous, lastDevice, last) =>
    increase(sample.loads?.takeover?.newConnections ?? undefined, last.loads.takeover?.newConnections ?? 0),
  terminalBytesPerSecond: (sample, previous) => perSecond(sample, previous, 'bytes',
    sample.loads?.osTerminal, previous?.loads?.osTerminal),
  // Places in the terminal's output where a number did not follow the one
  // before: the breaks since the sample before.
  terminalLinesLost: (sample, previous, lastDevice, last) => eventsSince(sample.loads?.osTerminal, last.loads.osTerminal, 'breaks'),
  // The round trips of the light terminal, in ms on the page's clock: from
  // the page's send of a line to the arrival of the host's answer to it in
  // the terminal's output, LINE_ANSWER_MS for a line without an answer by
  // then (lineTimesAt). The slowest of the round trips that the page judged
  // since the sample before (terminalLineMaxMs), and the 95th percentile of
  // those of the rule's "windowSeconds" (terminalLineP95Ms). No value in a
  // second without a judged round trip.
  terminalLineMaxMs: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share, keyframes, times) =>
    (times?.length ? Math.max(...times) : undefined),
  terminalLineP95Ms: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share, keyframes, times, p95) => p95,
  fileBytesPerSecond: (sample, previous) => perSecond(sample, previous, 'bytesSent',
    sample.loads?.osFiles, previous?.loads?.osFiles),
  // The three counts of transfers are those of the file transfer that is on,
  // osFiles or osFilesSmall (transfers()).
  // Transfers that the Mini reported as done and whose file on the host has
  // another SHA-256 than the file that the page sent.
  fileTransfersCorrupt: (sample, previous, lastDevice, last) => {
    const [now, before] = transfers(sample, last);
    return increase(now?.corrupt, before?.corrupt);
  },
  // Transfers whose channel the Mini closed before it reported their
  // outcome, since the sample before: the channel closed while the page's
  // connection to the Mini was up. The page shows such a transfer as
  // cancelled.
  // A restart of the Mini ends the transfer that is under way, which is not
  // one that the Mini closed.
  fileTransfersClosed: (sample, previous, lastDevice, last, held, restarting) => {
    const [now, before] = transfers(sample, last);
    return restarting ? undefined : increase(now?.closed, before?.closed);
  },
  // Transfers that broke off, or whose file on the host has another checksum.
  // A transfer whose channel closed when the Mini restarted is not one of
  // them; a transfer with a failed outcome or a wrong file in those seconds is.
  filesFailed: (sample, previous, lastDevice, last, held, restarting) => {
    const [now, before] = transfers(sample, last);
    if (!now || !before) return undefined;
    // A drop of the small transfer that the Mini did not take is a failed
    // one: that transfer is there for the start of a drop. The 1 MiB
    // transfer tries a refused drop again, and its rate judges the time.
    const refused = sample.loads?.osFilesSmall ? (now.refused ?? 0) - (before.refused ?? 0) : 0;
    return Math.max(0, now.failed - before.failed + refused + (restarting ? 0 : now.broken - before.broken));
  },
  // The clipboard rounds that ended since the sample before, each [startedAt,
  // ms, back] (os_services.mjs). Rounds in which the text did not come back
  // as it was set.
  clipboardWrong: (sample, previous, lastDevice, last) =>
    eventsSince(sample.loads?.osClipboard, last.loads.osClipboard, 'ended', ([, , back]) => back !== true),
  // Clipboard rounds in which the host answered with another text than the
  // one that was set; a round without an answer is not one of them.
  clipboardCorrupt: (sample, previous, lastDevice, last) =>
    eventsSince(sample.loads?.osClipboard, last.loads.osClipboard, 'ended', ([, , back]) => back === false),
  // The slowest clipboard round since the sample before, of the rounds in
  // which the host answered; a round that is under way counts with the time
  // it has taken by the sample, so that a slow round shows in every second
  // of it. No value in a second without either.
  clipboardMs: sample => {
    const clipboard = sample.loads?.osClipboard;
    if (!clipboard?.ended) return undefined;
    const times = clipboard.ended.filter(([, , back]) => back !== null).map(([, ms]) => ms);
    if (clipboard.runningSince !== null) times.push(clipboard.at - clipboard.runningSince);
    return times.length ? Math.max(...times) : undefined;
  },
  // 1 when the upload's request has ended, with an error or an answer; the
  // tool stops an upload before the Mini has the whole file.
  uploadEnded: sample => (sample.loads?.upload ? Number(Boolean(sample.loads.upload.error)) : undefined),
  uploadBytesPerSecond: (sample, previous) => perSecond(sample, previous, 'bytesSent',
    sample.loads?.upload, previous?.loads?.upload),
  // Keys whose report from the host's key watch the tool did not read
  // within KEY_LOST_MS of their send, counted in the first sample after
  // that time, so a change that pauses this measure pauses it for that much
  // longer (hostKeys() in hid_times.mjs).
  keysLost: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share, keyframes, times, p95, host) =>
    host?.keysLost,
  // Moves of the mouse that the host did not have MOVE_LOST_MS after the
  // page sent them, of those that the judge took at this sample
  // (MOVE_LOST_MS and HOST_LINE_GRACE_MS after their send).
  mouseMovesLost: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share, keyframes, times, p95, host) =>
    host?.mouseMovesLost,
  // The longest time, in ms on the host's clock, between two moves that the
  // host got of one movement of the hand, among the moves that the judge
  // took at this sample. No value in a second without such a pair.
  mouseGapMs: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share, keyframes, times, p95, host) =>
    host?.mouseGapMs,
  // The time, on the page's clock, from the page's send of a key to the
  // Mini's message that lists the key as down, KEY_ANSWER_MS for a key
  // without such a message by then (keyTimesAt): the slowest of the keys
  // that the tool judged since the sample before (keyLatencyMaxMs), and the
  // 95th percentile of those of the rule's "windowSeconds"
  // (keyLatencyP95Ms). The keys are those of hid or, without it, of
  // slowKeys, which has a key in two seconds of three: a second without a
  // judged key has no value.
  keyLatencyMaxMs: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share, keyframes, times) =>
    (times?.length ? Math.max(...times) : undefined),
  keyLatencyP95Ms: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone, share, keyframes, times, p95) => p95,
  // 1 when the Mini reports that an upload to the TF card holds the host's
  // sound off (audioPausedBy has "upload"), else 0. No value for firmware
  // that does not report what holds the sound off.
  audioPausedByUpload: sample => (sample.device?.audioPausedBy ? Number(sample.device.audioPausedBy.includes('upload')) : undefined),
  // The host's sound, which the Mini sends to the browser, from the
  // browser's report of the Mini's audio track (loads.audio). While the Mini
  // holds the sound off it sends no audio frame, by design, and the browser
  // plays silence: the measures of the sound in the browser have no value
  // then (`held`, from soundHeldOffAt). Audio packets that did not reach the
  // browser, since the report before.
  audioPacketsLost: (sample, previous, lastDevice, last, held) => (held ? undefined
    : increase(sample.loads?.audio?.packetsLost, last.loads.audio?.packetsLost)),
  // The sound that the browser made up since the report before, in ms,
  // because the Mini's sound for that time was not there when the browser
  // had to play it (concealedSamples, of which a second has 48000): a gap
  // that the listener hears as a short repeat, a fade or silence.
  audioConcealedMs: (sample, previous, lastDevice, last, held) => {
    if (held) return undefined;
    const samples = increase(sample.loads?.audio?.concealedSamples, last.loads.audio?.concealedSamples);
    return samples === undefined ? undefined : samples / 48;
  },
  // The level of the sound that the browser played since the report before,
  // 0 (silence) to 1 (levelBetween() in audio.mjs). The target page's tone
  // and beeps have about 0.15 at the full volume of the Mini's output on
  // the host.
  audioLevel: (sample, previous, lastDevice, last, held) => (held ? undefined
    : levelBetween(sample.loads?.audio, last.loads.audio)),
  // The strongest frequency of the sound that the browser played since the
  // sample before, in Hz, from the strongest frequency of each window of it
  // (middleTone() and toneFinder() in audio.mjs): the target page's tone
  // has 220 Hz. No value in a second without a window of sound.
  audioToneHz: (sample, previous, lastDevice, last, held) => (held ? undefined : middleTone(sample.loads?.audio?.tones)),
  // The mean time, in ms, that the sound played since the report before
  // waited in the browser's jitter buffer.
  audioJitterBufferMs: (sample, previous, lastDevice, last, held) => {
    if (held) return undefined;
    const now = sample.loads?.audio;
    const before = last.loads.audio;
    const emitted = now?.jitterBufferEmittedCount - before?.jitterBufferEmittedCount;
    return emitted > 0 ? (now.jitterBufferDelay - before.jitterBufferDelay) / emitted * 1000 : undefined;
  },
  // The time, in ms, from the host's output of a beep to the browser's
  // playing of it: the longest of the beeps heard since the sample before,
  // by the clocks of the target page and of the host that the sample has
  // (beepDelayMs() in audio.mjs). No value in a second without a beep, and
  // none before the target page's first report with a clock.
  audioDelayMs: (sample, previous, lastDevice, last, held) => {
    const audio = sample.loads?.audio;
    if (held || !audio?.beepsAt?.length || !audio.beepClock) return undefined;
    return Math.round(Math.max(...audio.beepsAt.map(at => beepDelayMs(at, audio.beepClock))));
  },
  // Opus frames, of 20 ms each, that the Mini encoded and did not send
  // because its queue of ten was full (audio.droppedFrames), since the
  // Mini's answer before. No value for firmware whose compact stats have no
  // audio counters.
  audioFramesDropped: (sample, previous, lastDevice) => grown(sample.device, lastDevice, device => device.audio?.droppedFrames),
  // The Mini's own counts of what failed, from the full stats
  // (sample.counters, memoryCounters() in recorder.mjs): how much each grew
  // since the answer before that had it, so that what fails beside a missing
  // answer is counted in the next one. Allocations that failed, in all of
  // the Mini's counters of them.
  allocationFailures: (sample, previous, lastDevice, last) => {
    const each = Object.keys(sample.counters?.allocationFailures ?? {})
      .map(place => grown(sample.counters, last.counters, counters => counters.allocationFailures?.[place]))
      .filter(count => count !== undefined);
    return each.length ? each.reduce((sum, count) => sum + count, 0) : undefined;
  },
  // Rebuilds of the video capture that failed.
  videoRebuildFailures: (sample, previous, lastDevice, last) =>
    grown(sample.counters, last.counters, counters => counters.videoRebuildFailures),
  // Reads of a mounted HTTPS image that the Mini refused: its counts of
  // refused requests of the TLS owners "https-media-range", which a read on
  // the image's open connection has, and "https-media-connect", which a read
  // has that must open the connection first. The Mini refuses such a request
  // when its free memory, less what its other TLS connections have reserved,
  // is under its floor; the host's read then fails. A Mini that has made no
  // such read yet does not list the owner, which is a count of 0.
  imageReadsRefused: (sample, previous, lastDevice, last) => grown(sample.counters, last.counters,
    counters => (counters.tlsAdmissions
      ? IMAGE_READ_OWNERS.reduce((sum, owner) => sum + (counters.tlsAdmissions[owner]?.denied ?? 0), 0) : undefined)),
  // Connections of the sender's update checks and downloads, which the
  // Mini opens to the OS Services release server for the sender: what its
  // count of refused requests of the TLS owner "osvc-update" grew by. The
  // Mini refuses such a request when its free memory, less what its other
  // TLS connections have reserved, is under its floor; the sender's update
  // check or download then fails. A Mini that has opened no such
  // connection yet does not list the owner, which is a count of 0.
  senderUpdatesRefused: (sample, previous, lastDevice, last) => grown(sample.counters, last.counters,
    counters => (counters.tlsAdmissions ? counters.tlsAdmissions['osvc-update']?.denied ?? 0 : undefined)),
  // Holds of a TLS reservation in which the Mini's free internal RAM or
  // DMA-capable RAM fell by more than everything that was reserved during
  // the hold and the room between the admission floor and the hard floor:
  // what the sum of the owners' counts (heldOverruns) grew by. Such a fall
  // takes free memory under the hard floor when the Mini admitted the
  // reservation at its limit, so a reservation is too small for its owner
  // or another task took more than the room between the floors. A Mini
  // that lists no owner yet has 0. An owner that was only refused has held
  // nothing and has no count. No value for firmware without the count.
  tlsHeldOverruns: (sample, previous, lastDevice, last) => grown(sample.counters, last.counters, counters => {
    if (!counters.tlsAdmissions) return undefined;
    const held = Object.values(counters.tlsAdmissions).filter(owner => owner.admitted > 0);
    if (held.some(owner => typeof owner.heldOverruns !== 'number')) return undefined;
    return held.reduce((sum, owner) => sum + owner.heldOverruns, 0);
  }),
  // Writes of an upload to the TF card that failed.
  uploadWriteErrors: (sample, previous, lastDevice, last) =>
    grown(sample.counters, last.counters, counters => counters.uploadWriteErrors),
  // The least free memory since the Mini's start, in bytes: internal RAM,
  // the part of it that DMA can use, and PSRAM. The Mini's memory budget
  // keeps each above a hard floor.
  freeInternalLowWater: sample => sample.counters?.freeInternalLowWater,
  freeDmaLowWater: sample => sample.counters?.freeDmaLowWater,
  freePsramLowWater: sample => sample.counters?.freePsramLowWater,
  // The seconds in a row, this one included, in which the Mini answered and
  // the page's session was not connected (sessionGoneAt).
  sessionGoneSeconds: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt, gone) => gone,
  // The Mini on the tool's MQTT broker (mqttAt).
  mqttVideoWrong: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt) => mqtt?.videoWrong,
  mqttSilentSeconds: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt) => mqtt?.silentSeconds,
  mqttFirstStatusSeconds: (sample, previous, lastDevice, last, held, restarting, lost, reserve, mqtt) => mqtt?.firstStatusSeconds,
  // The least memory that the update checks since the full answer before
  // left above the Mini's floors, in bytes (tlsReserveAt).
  tlsReserveMin: (sample, previous, lastDevice, last, held, restarting, lost, reserve) => reserve,
  // Update checks that the Mini refused or did not answer, since the sample
  // before: the checks of the sample that ended with an error ([startedAt,
  // ms, error], switches.mjs).
  updateChecksFailed: (sample, previous, lastDevice, last) =>
    eventsSince(sample.loads?.updateCheck, last.loads.updateCheck, 'checks', ([, , error]) => error !== null),
  // The internal RAM that a series of update checks beside video alone did
  // not give back, in bytes (memoryLostAt).
  memoryLostBytes: (sample, previous, lastDevice, last, held, restarting, lost) => lost,
};

// tlsReserveMin: the samples whose full answer has an update check that the
// Mini admitted since the full answer before, each with the least memory
// that such a check left above the Mini's floors: the smaller of internal
// RAM and DMA-capable RAM. The Mini reports this per TLS owner, and an
// update check has the owner "ota-download", as a firmware download has. It
// reports what the newest request left (lastInternalHeadroom,
// lastDmaHeadroom) and the least since its start (minInternalHeadroom,
// minDmaHeadroom). After one check the value is what that check left. After
// several, the answer does not show the earlier ones, so a least value that
// is lower than in the full answer before counts as well: a check since that
// answer set it. A least value that did not change is older, perhaps of a
// state in which the measure has no value, and is not judged here. After a
// restart of the Mini every least value is from since that answer. The
// checks are those of updateCheck: the variable was on in a sample since the
// answer before, so the last check of a state is judged in the first answer
// after it. No value when a HTTPS image was mounted in one of those samples:
// a read holds a reservation and a connection of its own, so a check leaves
// less there by design.
function tlsReserveAt(samples) {
  const reserve = new Map();
  const update = counters => counters.tlsAdmissions?.['ota-download'];
  const numbers = (...values) => values.filter(value => typeof value === 'number');
  let before = null;        // the full answer before
  let checking = false;     // updateCheck was on since then
  let reading = false;      // a HTTPS image was mounted since then
  for (const sample of samples) {
    checking ||= Boolean(sample.state.updateCheck);
    reading ||= Boolean(sample.state.httpsMountDisk);
    const now = sample.counters;
    if (!now?.tlsAdmissions) continue;
    const [to, from] = [update(now), before && update(before)];
    const restarted = before && now.nowMs < before.nowMs;
    const admitted = before ? (to?.admitted ?? 0) - (restarted ? 0 : from?.admitted ?? 0) : 0;
    if (admitted > 0 && checking && !reading) {
      // A least value counts when a check since the answer before set it.
      const lower = (value, earlier) => (restarted || typeof earlier !== 'number' || value < earlier ? value : undefined);
      const left = numbers(to.lastInternalHeadroom, to.lastDmaHeadroom,
        ...(admitted > 1 ? [lower(to.minInternalHeadroom, from?.minInternalHeadroom), lower(to.minDmaHeadroom, from?.minDmaHeadroom)] : []));
      if (left.length) reserve.set(sample, Math.min(...left));
    }
    before = now;
    checking = Boolean(sample.state.updateCheck);
    reading = Boolean(sample.state.httpsMountDisk);
  }
  return reserve;
}

// The settled level of the Mini's free internal RAM is the highest of its
// answers over a time: other tasks take some of it for moments, which only
// lowers an answer. Before a series of update checks it is taken from the
// answers of this long.
const SETTLED_BEFORE_MS = 10_000;
// The least number of checks that makes a series, so that a small leak per
// check adds up past the 1 KiB limit.
const SERIES_CHECKS = 5;

// memoryLostBytes: the samples that end a series of update checks, each with
// what the series lost. A series is a run of samples with updateCheck and
// video on and nothing else, between two runs with video alone; mqtt may be
// on beside them in all three runs, where the Mini's connection to the
// broker is in the level before and in the level after. The loss is
// the settled free internal RAM (counters.freeInternal) of the last
// SETTLED_BEFORE_MS before the series less that of the whole run after it,
// in which the Mini frees what the last check still held. It is the value
// of the last sample of that run. A series without a full answer in that
// time before it or in the run after it, or with fewer than SERIES_CHECKS
// checks that the Mini admitted between those answers, has no measured
// loss: its value is Infinity, which fails every limit. No value when the
// Mini restarted between the two runs.
function memoryLostAt(samples) {
  const lost = new Map();
  const on = sample => Object.keys(sample.state).filter(name => sample.state[name]).sort().join();
  const runs = [];
  for (const sample of samples) {
    if (runs.at(-1)?.on === on(sample)) runs.at(-1).samples.push(sample);
    else runs.push({ on: on(sample), samples: [sample] });
  }
  const answers = list => list.map(sample => sample.counters).filter(counters => typeof counters?.freeInternal === 'number');
  runs.forEach((series, index) => {
    const [before, after] = [runs[index - 1], runs[index + 1]];
    const mqtt = series.on.startsWith('mqtt,') ? 'mqtt,' : '';
    if (series.on !== `${mqtt}updateCheck,video` || before?.on !== `${mqtt}video` || after?.on !== `${mqtt}video`) return;
    const from = answers(before.samples.filter(sample => sample.t > series.samples[0].t - SETTLED_BEFORE_MS));
    const to = answers(after.samples);
    const clock = list => list.find(sample => sample.device)?.device.nowMs;
    if (clock(after.samples) < clock(before.samples)) return;
    const settled = list => Math.max(...list.map(counters => counters.freeInternal));
    const admitted = counters => counters?.tlsAdmissions?.['ota-download']?.admitted ?? 0;
    const measured = from.length && to.length && admitted(to.at(-1)) - admitted(from.at(-1)) >= SERIES_CHECKS;
    lost.set(after.samples.at(-1), measured ? settled(from) - settled(to) : Infinity);
  });
  return lost;
}

// The three MQTT measures of each sample in which the Mini has the tool's
// broker (loads.mqtt, mqtt.mjs): { videoWrong, silentSeconds,
// firstStatusSeconds }.
//
// The Mini's connection to the broker begins when the Mini gets the broker
// (since), and again at each start of the Mini after that. A message that
// the broker got before that beginning says nothing about the Mini now, so
// only later messages count. A sample without the Mini's answer does not
// show a restart; the next answer does, by its smaller clock.
//
// firstStatusSeconds: from that beginning to the Mini's first status
// message, in the sample that has the message. In each sample before it,
// the seconds so far, so that a Mini that never sends one breaks a limit.
// No value after the message.
//
// silentSeconds: since the Mini's newest status message. The Mini publishes
// its states every 15 s, with its status first. No value before the first
// one, which firstStatusSeconds waits for.
//
// videoWrong: the Mini's newest video/state message against the Mini's
// answer of the same sample. 1 when the message's "ready" is not the Mini's
// modeValid, or, with a mode, its width or height is not the Mini's; else 0.
// The Mini publishes a new mode within a second and a lost signal after 3 s.
// While it switches between its two video sources it has no mode and
// publishes nothing, so that the broker keeps the last values: no value
// then, and none without an answer or without a message.
function mqttAt(samples) {
  const values = new Map();
  let clock;               // the Mini's clock in its newest answer
  let started = 0;         // the start of the Mini's clock, from its newest answer
  let restarted = false;   // the Mini started since the beginning of the connection
  let since;               // loads.mqtt.since of the connection
  let from;                // the beginning of the Mini's connection
  let first = false;       // a status message has come since then
  for (const sample of samples) {
    const mode = sample.device;
    if (mode) {
      restarted ||= mode.nowMs < clock;
      [clock, started] = [mode.nowMs, sample.t - mode.nowMs];
    }
    const mqtt = sample.loads?.mqtt;
    if (typeof mqtt?.since !== 'number') continue;
    if (mqtt.since !== since || restarted) {
      since = mqtt.since;
      from = Math.max(since, started);
      first = false;
      restarted = false;
    }
    const value = {};
    if (!first) {
      first = mqtt.statusAt >= from;
      value.firstStatusSeconds = ((first ? mqtt.statusAt : mqtt.at) - from) / 1000;
    }
    if (mqtt.statusAt >= from) value.silentSeconds = (mqtt.at - mqtt.statusAt) / 1000;
    const said = mqtt.videoAt >= from ? mqtt.video : null;
    const switching = mode && !mode.modeValid && mode.guestVideo?.source?.startsWith('switching_');
    if (said && typeof mode?.modeValid === 'boolean' && !switching) {
      value.videoWrong = Number(said.ready !== mode.modeValid ||
        (mode.modeValid && (said.width !== mode.width || said.height !== mode.height)));
    }
    values.set(sample, value);
  }
  return values;
}

// The samples in whose second the Mini held the host's sound off. Its answer
// says so when it names a source in audioPausedBy. A hold begins and ends
// between two answers, and the Mini often does not answer during an upload
// over Wi-Fi. So a sample without an answer counts when the answer before
// or the next answer names a source, and so does the first sample after
// such a time, whose second began under the hold.
//
// The Mini can also be without an answer for a whole upload, and then no
// answer names the hold. holdExpected(sample) says whether the board's rules
// expect the Mini to hold the sound off in the sample's state. In such a
// state a sample without an answer counts when the browser received no audio
// packet since its report before: the Mini sends no audio frame while it
// holds the sound off, and a gap in a sound that it sends still has packets.
function soundHeldOffAt(samples, holdExpected = () => false) {
  const held = new Set();
  const names = sample => Boolean(sample.device?.audioPausedBy?.length);
  let before = false;      // the newest answer names a source
  let unanswered = [];     // the samples without an answer since then
  let heard;               // the browser's newest report of the sound before the sample
  for (const sample of samples) {
    const audio = sample.loads?.audio;
    const noPacket = audio && heard && audio.packetsReceived !== undefined && audio.packetsReceived === heard.packetsReceived;
    if (audio) heard = audio;
    if (!sample.device) {
      if (before || (noPacket && holdExpected(sample))) held.add(sample);
      unanswered.push(sample);
      continue;
    }
    // The first answer without a source after one with a source: the hold
    // ended in this sample's second or in an unanswered one before it.
    if (names(sample) || before) for (const one of [sample, ...unanswered]) held.add(one);
    before = names(sample);
    unanswered = [];
  }
  return held;
}

// The Mini closes its connections some seconds before its clock starts
// again: a sample this long before that start is in the restart.
const RESTART_MS = 5000;

// The samples in whose second the Mini restarted, or was about to: those
// from RESTART_MS before the start of the Mini's clock to the first answer
// after it. The clock's start is the time of that answer less the Mini's
// clock in it.
function restartingAt(samples) {
  const restarting = new Set();
  let before = null;   // the newest answer before the sample
  for (const sample of samples) {
    if (!sample.device) continue;
    if (before && sample.device.nowMs < before.device.nowMs) {
      const from = sample.t - sample.device.nowMs - RESTART_MS;
      for (const one of samples) if (one.t >= from && one.t <= sample.t) restarting.add(one);
    }
    before = sample;
  }
  return restarting;
}

// The TLS owners of the Mini's reads of a mounted HTTPS image.
const IMAGE_READ_OWNERS = ['https-media-range', 'https-media-connect'];

// The report of the file transfer that is on in a sample, and the newest
// report of the same variable before it.
function transfers(sample, last) {
  const name = sample.loads?.osFilesSmall ? 'osFilesSmall' : 'osFiles';
  return [sample.loads?.[name], last.loads[name]];
}

// The browser's video report of a sample and the one before it, when both
// are of one stream in one session of the page; else nothing.
function sameStream(sample, last) {
  const now = sample.browser;
  const same = now?.video && last.video && now.peer !== undefined && now.peer === last.peer &&
    now.video.ssrc === last.video.ssrc && now.video.id === last.video.id;
  return same ? [now.video, last.video] : [];
}

// keyframesPerSecond looks back this long: a single second of the browser's
// reports has one keyframe more or less than the Mini sent in it.
export const KEYFRAME_SECONDS = 3;

// keyframesPerSecond: for each sample, the keyframes that the browser
// decoded between its report KEYFRAME_SECONDS samples before and this one,
// per second of the time between the two reports, when both are of one
// stream in one session of the page.
function keyframesAt(samples) {
  const rate = new Map();
  samples.forEach((sample, index) => {
    const [now, before] = [sample.browser, samples[index - KEYFRAME_SECONDS]?.browser];
    if (!now?.video || !before?.video) return;
    if (now.peer === undefined || now.peer !== before.peer || now.video.ssrc !== before.video.ssrc || now.video.id !== before.video.id) return;
    const seconds = (now.video.timestamp - before.video.timestamp) / 1000;
    if (seconds > 0) rate.set(sample, Math.max(0, now.video.keyFramesDecoded - before.video.keyFramesDecoded) / seconds);
  });
  return rate;
}

// framesDecodedShare looks back this long: one frame more or less in a
// single second is 3 % of its 30.
export const DECODED_SHARE_SECONDS = 10;

// framesDecodedShare: for each sample, the frames that the browser decoded
// in the DECODED_SHARE_SECONDS before it, divided by the frames that the
// Mini sent in that time (accessUnitsSent of its answers). It has a value
// when the sample and the one that long before it have the browser's report
// of the same stream of the same session and the Mini's answer, and the Mini
// did not restart and sent a frame between them.
function decodedShareAt(samples) {
  const share = new Map();
  samples.forEach((sample, index) => {
    const from = samples[index - DECODED_SHARE_SECONDS];
    const [now, before] = [sample.browser, from?.browser];
    if (!now?.video || !before?.video || !sample.device || !from.device) return;
    if (now.peer === undefined || now.peer !== before.peer || now.video.ssrc !== before.video.ssrc || now.video.id !== before.video.id) return;
    const sent = sample.device.accessUnitsSent - from.device.accessUnitsSent;
    // The frames between the browser's two reports, for the time between the Mini's two answers.
    const seconds = (sample.device.nowMs - from.device.nowMs) / 1000;
    const reported = (now.video.timestamp - before.video.timestamp) / 1000;
    if (!(sent > 0) || !(seconds > 0) || !(reported > 0)) return;
    share.set(sample, (now.video.framesDecoded - before.video.framesDecoded) / reported / (sent / seconds));
  });
  return share;
}

// sessionGoneSeconds: for each sample, the number of seconds in a row, up to
// and with its own, in which the Mini answered and the page's WebRTC session
// was not connected: a session that died on a Mini that is up. A second
// without an answer of the Mini has no value and leaves the count as it is.
// A restart of the Mini ends the session by design: the Mini does not
// answer while it restarts, and from its first answer after the restart,
// which its clock shows, there is no value until the page's session is
// connected again: the time of that way back is the change's. A second
// before the restart in which the Mini still answers is counted.
function sessionGoneAt(samples) {
  const gone = new Map();
  let seconds = 0;
  let afterRestart = false;
  let before = null;   // the Mini's newest answer
  for (const sample of samples) {
    if (sample.device && before && sample.device.nowMs < before.nowMs) afterRestart = true;
    if (sample.device) before = sample.device;
    if (sample.browser?.connectionState === 'connected') {
      seconds = 0;
      afterRestart = false;
    }
    if (afterRestart || !sample.device) continue;
    if (sample.browser?.connectionState !== 'connected') seconds++;
    gone.set(sample, seconds);
  }
  return gone;
}

// The sample of the mount that is on, whichever variable mounted it.
const mount = sample => Object.values(sample?.loads ?? {}).find(load => load?.bytesRead !== undefined);

// How much a count of the host's reader grew since the report before of the
// mount that is on. The reader counts from the mount on, so the mount's
// first report is compared with zero.
function sinceMount(sample, last, count) {
  const [name, now] = Object.entries(sample.loads ?? {}).find(([, load]) => load?.bytesRead !== undefined) ?? [];
  return now?.[count] === undefined ? undefined : Math.max(0, now[count] - (last.loads[name]?.[count] ?? 0));
}

// The mean time in milliseconds of the frames between two answers of the
// Mini, from its counters since boot: the counters of the frames, `frames`,
// and the sum of their times, `us`. A Mini that restarted counts from zero
// again, so its answer has the frames since the restart. Undefined without
// a new frame, and for firmware that has no such counters.
function encodeMean(device, answerBefore, frames, us) {
  const now = device?.videoEncode;
  if (!now || !answerBefore) return undefined;
  const restarted = device.nowMs < answerBefore.nowMs;
  const since = name => now[name] - (restarted ? 0 : answerBefore.videoEncode?.[name]);
  const count = frames.reduce((sum, name) => sum + since(name), 0);
  const time = since(us);
  return count > 0 && time >= 0 ? time / count / 1000 : undefined;
}

// The longest encode time in the Mini's answer, in microseconds: 0 when the
// Mini encoded no frame in its two windows, NaN without the counters.
function longestEncodeUs(sample) {
  const encode = sample.device?.videoEncode;
  return Math.max(encode?.pEncodeMaxUs, encode?.idrEncodeMaxUs);
}

// hid presses a key every KEY_PERIOD_MS. A key that the Mini has not
// answered KEY_ANSWER_MS after its send has no answer, and counts with that
// time. A key is lost when the tool has not read the host's report of it
// KEY_LOST_MS after its send (hostKeys() in hid_times.mjs). A key of
// HID_KEYS comes again after the others, 3.6 s later, so both times end
// before the same key is sent again.
export const KEY_PERIOD_MS = 100;
export const KEY_ANSWER_MS = 3000;
export const KEY_LOST_MS = 3000;
// The light terminal sends a line every LINE_PERIOD_MS. A line without the
// host's answer LINE_ANSWER_MS after its send has no answer, and counts
// with that time (startTerminal() in os_services.mjs).
export const LINE_PERIOD_MS = 100;
export const LINE_ANSWER_MS = 3000;
// A move of the mouse is lost when the host does not have it this long after
// the page sent it.
export const MOVE_LOST_MS = 1000;

// The judge looks for a move this long after it would be lost
// (hostInputAt() in hid_times.mjs): the line of the host's key watch for
// that time has come by then.
export const HOST_LINE_GRACE_MS = 300;

// The time before a sample that the sample's value reports, for a measure
// that reports more than the second before it: a frame's time stays in the
// Mini's longest times for up to two of its 2-second windows; a key's
// answer is in the sample in which the tool took it from the page, which it
// does once per key period, and a key without an answer in the sample after
// KEY_ANSWER_MS; a line of the light terminal is in the sample after its
// answer, or after LINE_ANSWER_MS without one; a lost key is counted
// KEY_LOST_MS after its send; and a move of the mouse is in the sample in
// which the judge looks for it. A 95th percentile has the time of its kind's
// slowest time: a sample in which a change pauses it adds no time to its
// window.
const moveJudgedMs = MOVE_LOST_MS + HOST_LINE_GRACE_MS;
const keyJudgedMs = 1000 + KEY_ANSWER_MS + KEY_PERIOD_MS;
const lineJudgedMs = 1000 + LINE_ANSWER_MS;
const reportsMs = { encodeMaxMs: 4000, encodeHandoffMaxMs: 4000,
  keyframesPerSecond: KEYFRAME_SECONDS * 1000, framesDecodedShare: DECODED_SHARE_SECONDS * 1000,
  keyLatencyMaxMs: keyJudgedMs, keyLatencyP95Ms: keyJudgedMs, terminalLineMaxMs: lineJudgedMs, terminalLineP95Ms: lineJudgedMs,
  keysLost: 1000 + KEY_LOST_MS, mouseMovesLost: 1000 + moveJudgedMs, mouseGapMs: 1000 + moveJudgedMs };
// The measures of single times, by kind: the keys to the Mini's answer
// (key), and the round trips of the light terminal's lines (line). Each
// kind has the slowest time of a sample, and the 95th percentile over its
// rule's window, whose rule has "windowSeconds". The report's line of a
// breach of one of them has the counts of its times beside it.
const TIMED = { keyLatencyMaxMs: 'key', keyLatencyP95Ms: 'key', terminalLineMaxMs: 'line', terminalLineP95Ms: 'line' };
const PERCENTILES = ['keyLatencyP95Ms', 'terminalLineP95Ms'];
const KINDS = {
  key: { one: 'key', many: 'keys', answerMs: KEY_ANSWER_MS },
  line: { one: 'line', many: 'lines', answerMs: LINE_ANSWER_MS },
};
// The 95th percentile of times, by the nearest rank: the time that 95 % of
// them do not exceed.
export const percentile95 = times => [...times].sort((a, b) => a - b)[Math.ceil(times.length * 0.95) - 1];

// A time to an answer, or null when the answer did not come within
// answerMs: the walk takes no answer after its own wait, and an answer after
// a shorter answerMs of the judge is none either.
const answeredWithin = (ms, answerMs) => (ms === null || ms > answerMs ? null : ms);

// The keys that the tool judged in each sample: Map(sample -> [ms, ...]),
// the time of each key to the Mini's answer, null for a key without an
// answer within KEY_ANSWER_MS. The keys are those of hid or, without it, of
// slowKeys; a sample of either has its judged keys as [usage, sentAt,
// latencyMs] (switches.mjs).
function keyTimesAt(samples) {
  return new Map(samples.map(sample => {
    const keys = (sample.loads?.hid ?? sample.loads?.slowKeys)?.keys;
    return [sample, keys ? keys.map(([, , latencyMs]) => answeredWithin(latencyMs, KEY_ANSWER_MS)) : []];
  }));
}

// The samples in which the page's session was connected. A sample has no
// report of the browser when the page has no peer connection, and also when
// the page did not answer in time. The page makes a new peer connection, with
// a new name (peer), for each session, so a sample without a report between
// two reports of the same connected peer connection had that session.
function connectedAt(samples) {
  const connected = new Set();
  let before = null;   // the newest report
  let missing = [];    // the samples without a report since it
  for (const sample of samples) {
    const report = sample.browser;
    if (!report) {
      missing.push(sample);
      continue;
    }
    if (report.connectionState === 'connected') {
      connected.add(sample);
      if (report.peer && before?.connectionState === 'connected' && before.peer === report.peer) missing.forEach(m => connected.add(m));
    }
    before = report;
    missing = [];
  }
  return connected;
}

// The round trips of the light terminal that the page judged in each
// sample: Map(sample -> [ms, ...]), null for a line without an answer
// within LINE_ANSWER_MS. A restart of the Mini ends the terminal with the
// page's session, and the lines of that time get no answer, which is not a
// slow session: a sample of the seconds of a restart, or one in which the
// page's session is not connected (connectedAt), has none.
function lineTimesAt(samples, restarting) {
  const connected = connectedAt(samples);
  return new Map(samples.map(sample => {
    const trips = sample.loads?.osTerminalLight?.roundTrips;
    if (!trips || restarting.has(sample) || !connected.has(sample)) return [sample, []];
    return [sample, trips.map(([, ms]) => answeredWithin(ms, LINE_ANSWER_MS))];
  }));
}
// The measures over a window of earlier samples, whose rules leave states out.
const windowed = ['keyframesPerSecond', 'framesDecodedShare'];

// The measures that are the Mini's condition at the instant of its answer. A
// sample has the state of the time at which the Mini was asked, and the
// answer can be up to the request's timeout later. When a change began in
// between, the answer is the Mini's condition after the start of that
// change. It is not judged when that change puts the measure under another
// rule: the state before it then expects another value than the state
// after. A change that leaves the measure's rule as it is changes nothing.
const conditionAtAnswer = ['serviceVideo', 'videoPausedByUpload', 'audioPausedByUpload'];

// The measures that come from the Mini's short answer (device). An answer
// without a value of such a measure says that the Mini had nothing of it to
// report, as no HDMI frame to encode during service video, so it ends a
// breach of the measure. A measure of the browser, of a switch or of the
// full answer can have no value beside the Mini's answer, as between two
// slow keys, and keeps its breach.
const fromAnswer = new Set(['deviceRestarts', 'encodeMs', 'keyframeEncodeMs', 'encodeMaxMs', 'encodeHandoffMs',
  'encodeHandoffMaxMs', 'serviceVideo', 'videoPausedByUpload', 'framesTooBig', 'framesSkipped', 'audioPausedByUpload',
  'audioFramesDropped']);

// The time at which the Mini made each answer, on this machine's clock: the
// Mini's clock in the answer, plus the greatest difference between a
// sample's time and the Mini's clock in its answer since the Mini's last
// restart. An answer is never made before its sample's time, so the answer
// that came fastest gives that difference, short by its own way.
function answerTimes(samples) {
  const times = new Map();
  let boot = [];
  const close = () => {
    const ahead = Math.max(...boot.map(sample => sample.t - sample.device.nowMs));
    for (const sample of boot) times.set(sample, sample.device.nowMs + ahead);
    boot = [];
  };
  for (const sample of samples) {
    if (typeof sample.device?.nowMs !== 'number') continue;
    if (boot.length && sample.device.nowMs < boot.at(-1).device.nowMs) close();
    boot.push(sample);
  }
  if (boot.length) close();
  return times;
}

// The time in ms between two times of a switch's report, `from` and `to`,
// in the first sample whose report has `to`: undefined when the report
// before has the same `to`, and when one of the two times is not there.
const once = (now, before, from, to) => (typeof now?.[to] === 'number' && typeof now[from] === 'number' && now[to] !== before?.[to]
  ? now[to] - now[from] : undefined);

// How much a count grew since the report before.
const increase = (now, before) => (now === undefined || before === undefined ? undefined : Math.max(0, now - before));

// The events of the list `list` of a switch's report that match `which`:
// those since the report before. undefined without the list, and without a
// report before it, as for a count that grows (increase): the first report
// of a switch has no report before it to say what came in its second.
const eventsSince = (now, before, list, which = () => true) =>
  (now?.[list] === undefined || before === undefined ? undefined : now[list].filter(which).length);

// The longest time in ms between two frames that the page presented, from
// the page's frame times (frameTimes() in recorder.mjs): between the frame
// before the sample's frames and the first of them, between each two of
// them, and from the newest frame to the page's clock at its report.
// undefined before the page's first frame.
function longestFrameGap(frames) {
  if (!frames) return undefined;
  const times = [...(frames.before === null ? [] : [frames.before]), ...frames.times];
  if (!times.length) return undefined;
  let longest = frames.now - times.at(-1);
  for (let i = 1; i < times.length; i++) longest = Math.max(longest, times[i] - times[i - 1]);
  return Math.round(longest);
}

// How much a counter of the Mini grew since the Mini's answer before;
// counter() takes it from an answer. A Mini that has restarted, which its
// clock or a lower count shows, counts from zero again: the count is then
// what it grew by.
function grown(now, before, counter) {
  const [to, from] = [now && counter(now), before && counter(before)];
  if (to === undefined || to === null || from === undefined || from === null) return undefined;
  return now.nowMs < before.nowMs || to < from ? to : to - from;
}

// The whole seconds of the Mini's clock between two of its answers, at
// least 1; after a restart, the seconds since the restart.
function secondsBetween(now, before) {
  const ms = now.nowMs < before.nowMs ? now.nowMs : now.nowMs - before.nowMs;
  return Math.max(1, Math.round(ms / 1000));
}

// The rate of the counter `name` between a source's reports in two samples.
// A counter that is missing now counts as no progress; one that starts again
// from zero counts as none too. The time is the time between the two
// reports (`clock`, a field of the report), which a slow answer moves away
// from the time of the samples; a report without it has the sample's time.
function perSecond(sample, previous, name, now, before, clock = 'at') {
  if (now?.[name] === undefined) return 0;
  if (before?.[name] === undefined) return undefined;
  const seconds = ((now[clock] ?? sample.t) - (before[clock] ?? previous.t)) / 1000;
  return seconds > 0 ? Math.max(0, now[name] - before[name]) / seconds : undefined;
}

const changeName = change => `${Object.keys(change)[0]} ${Object.values(change)[0] ? 'on' : 'off'}`;
const holds = (where, state) => Object.entries(where).every(([name, value]) =>
  (typeof value === 'boolean' ? Boolean(state[name]) === value : state[name] === value));
const withinLimit = (line, value) => (line.min ?? -Infinity) <= value && value <= (line.max ?? Infinity);

function check(what, index, line, variables, keys) {
  const fail = text => { throw new Error(`${what} ${index + 1}: ${text}: ${JSON.stringify(line)}`); };
  const unknownKey = Object.keys(line).find(key => !keys.includes(key));
  if (unknownKey) fail(`unknown key "${unknownKey}"`);
  const unknown = [...Object.keys(line.while ?? {}), ...Object.keys(line.change ?? {})].find(name => !variables.includes(name));
  if (unknown) fail(`unknown variable "${unknown}"`);
  if (line.change && (Object.keys(line.change).length !== 1 || typeof Object.values(line.change)[0] !== 'boolean')) {
    fail('a change names one variable, true or false');
  }
  return fail;
}
// The rules or entries that `lines` stand for: one per variable of
// "andOneOf", added to "while", and one per measure of a list.
export function expand(lines) {
  return lines.flatMap(({ andOneOf, ...line }) => {
    const whiles = andOneOf ? andOneOf.map(name => ({ ...line.while, [name]: true })) : [line.while];
    const measuresOf = Array.isArray(line.measure) ? line.measure : [line.measure];
    return whiles.flatMap(where => measuresOf.map(measure => {
      const one = { ...line };
      if (where !== undefined) one.while = where;
      if (measure !== undefined) one.measure = measure;
      return one;
    }));
  });
}
function checkLists(what, index, line) {
  const fail = text => { throw new Error(`${what} ${index + 1}: ${text}: ${JSON.stringify(line)}`); };
  if (line.andOneOf !== undefined && (!Array.isArray(line.andOneOf) || line.andOneOf.length === 0 || line.change)) {
    fail('"andOneOf" is a list of variables, beside "while"');
  }
  if (Array.isArray(line.measure) && line.measure.length === 0) fail('"measure" is an empty list');
}

const sameChange = (a, b) => Object.keys(a)[0] === Object.keys(b)[0] && Object.values(a)[0] === Object.values(b)[0];

// The time at which a change's switch was done: the variable had its new
// value and worked. The row of a change whose switch was not done has the
// time at which the driver's wait for it was over.
const switchedAt = change => (typeof change.switchSeconds === 'number' ? change.startedAt + change.switchSeconds * 1000 : change.t);

/**
 * The longest that a change may take to settle, in seconds: the "max" of
 * its rule in changeTakesAtMost, or the larger "max" of a known failure's
 * entry for the settling time of that change in that state, on that host.
 * The driver waits this long, and its margin, for the change's switch.
 * undefined when neither names the change. state is the state from the
 * start of the change; host is "linux", "macos" or "windows".
 */
export function changeLimit(criteria, known, change, state, host) {
  const rule = criteria.changeTakesAtMost?.find(line => sameChange(line.change, change));
  const entries = expand(known).filter(entry => entry.change && entry.measure === undefined &&
    (entry.host === undefined || entry.host === host) && sameChange(entry.change, change) && holds(entry.while ?? {}, state));
  const limits = [rule?.max, ...entries.map(entry => entry.max)].filter(limit => typeof limit === 'number');
  return limits.length ? Math.max(...limits) : undefined;
}

/**
 * The line of a run's output for the row of a change. max: the "max" of the
 * change's rule, which the line names when the settling time is over it.
 */
export function changeLine(row, max) {
  const seconds = `${row.seconds.toFixed(1)} s`;
  const outcome = row.error ?? (row.unfinished ? `not finished after ${seconds}: ${row.unfinished}`
    : row.unsettled ? `did not settle in ${(row.seconds - row.switchSeconds).toFixed(0)} s: ${row.unsettled}`
      : row.endedBy ? `ended by ${row.endedBy} on, off after ${seconds}`
        : `settled after ${seconds}${row.seconds > max ? `, limit ${max} s` : ''}`);
  return `${changeName(row.change)}: ${outcome}`;
}

/** Throws if the rules cannot be judged. variables: the names a state can have. */
export function validateCriteria(criteria, variables) {
  const unknownPart = Object.keys(criteria).find(key => !['everySecond', 'changeTakesAtMost'].includes(key));
  if (unknownPart) throw new Error(`criteria: unknown part "${unknownPart}"`);
  (criteria.everySecond ?? []).forEach((line, index) => checkLists('everySecond rule', index, line));
  expand(criteria.everySecond ?? []).forEach((line, index) => {
    const fail = check('everySecond rule', index, line, variables, ['while', 'measure', 'min', 'max', 'failAfterSeconds', 'windowSeconds']);
    if (!(line.measure in measures)) fail(`unknown measure "${line.measure}"`);
    if (typeof line.min !== 'number' && typeof line.max !== 'number') fail('no min or max');
    if (PERCENTILES.includes(line.measure) !== (line.windowSeconds !== undefined)) {
      fail(`"windowSeconds" belongs to a rule of ${PERCENTILES.join(' or ')}, and such a rule has it`);
    }
    if (line.windowSeconds !== undefined && !(Number.isInteger(line.windowSeconds) && line.windowSeconds >= 1)) {
      fail('"windowSeconds" is a whole number of seconds, 1 or more');
    }
  });
  (criteria.changeTakesAtMost ?? []).forEach((line, index) => {
    const fail = check('changeTakesAtMost rule', index, line, variables, ['change', 'max', 'pauses', 'pauseSeconds']);
    if (!line.change || typeof line.max !== 'number') fail('no change or max');
    if (line.pauses !== undefined && !Array.isArray(line.pauses)) fail('"pauses" is a list of measures');
    const unknown = (line.pauses ?? []).find(measure => !(measure in measures));
    if (unknown !== undefined) fail(`unknown measure "${unknown}"`);
    if (line.pauseSeconds !== undefined && (typeof line.pauseSeconds !== 'number' || line.pauseSeconds < 0 || !line.pauses)) {
      fail('"pauseSeconds" is a number of seconds, 0 or more, beside "pauses"');
    }
  });
}

/** Throws if an entry of the known failures cannot be judged. */
export function validateKnown(known, variables) {
  known.forEach((entry, index) => checkLists('known failure', index, entry));
  expand(known).forEach((entry, index) => {
    const fail = check('known failure', index, entry, variables, ['issue', 'host', 'fails', 'change', 'while', 'min', 'max', 'measure', 'tolerateSeconds', 'recoverySeconds']);
    if (entry.fails !== undefined && entry.fails !== true) fail('"fails" is true, or the entry has no "fails"');
    if (typeof entry.issue !== 'string' || !entry.issue) fail('no issue');
    if (entry.host !== undefined && !HOST_NAMES.includes(entry.host)) fail(`"host" is one of ${HOST_NAMES.join(', ')}`);
    if (entry.change !== undefined && entry.measure === undefined) {
      // The time the change takes.
      if (typeof entry.max !== 'number' || entry.min !== undefined) {
        fail('an entry with "change" and no "measure" is for the time the change takes: it has "max", in seconds, and no "min"');
      }
      if (entry.tolerateSeconds !== undefined || entry.recoverySeconds !== undefined) {
        fail('an entry with "change" and no "measure" is for the time the change takes; "tolerateSeconds" and "recoverySeconds" belong to an entry with "measure"');
      }
    } else {
      // A measure, while a state holds or during a change.
      const form = entry.change !== undefined ? 'an entry with "change" and "measure" is for that measure during the change: ' : '';
      if (!entry.while && !entry.change) fail('no while or change');
      if (!(entry.measure in measures)) fail(`${form}unknown measure "${entry.measure}"`);
      if (typeof entry.tolerateSeconds !== 'number') fail(`${form}no tolerateSeconds`);
      if (entry.recoverySeconds !== undefined && typeof entry.recoverySeconds !== 'number') fail(`${form}recoverySeconds is not a number`);
    }
  });
}

/**
 * The exit status for the way a run ended: 3 when it failed with known
 * faults only, 4 when the recording cannot judge a change and the walk must
 * run again.
 */
export const EXIT = { pass: 0, fail: 1, undecided: 1, 'known faults': 3, 'walk again': 4 };

/** The directory of a board's criteria and known failures, by the bench file's JETKVM_SKU. */
export const BOARDS = { 'jetkvm-mini-ethernet': 'ethernet', 'jetkvm-mini-wireless': 'wifi' };

const isSample = row => 'device' in row;

// A session needs some seconds to recover from lost packets, so a breach can
// begin up to this long after the second in which the browser counted them.
// The browser counts a packet as lost when a later packet has come, so after
// a time without any packet the count comes at its end, and a breach can
// also be reported up to this long before it. The report names the seconds
// with lost video packets beside the failures that are that near.
const LOSS_SHOWS_FOR_MS = 5000;

// The seconds in which the browser counted video packets as lost, with the
// count, whatever the rules say about it: [{ t, packets }].
function lostPackets(samples) {
  const lost = [];
  let before;
  for (const sample of samples) {
    const video = sample.browser?.video;
    const packets = increase(video?.packetsLost, before?.packetsLost);
    if (packets > 0) lost.push({ t: sample.t, packets });
    if (video) before = video;
  }
  return lost;
}

// The rule that judges this measure in this sample: { line }, { tie } when
// two rules apply equally, or null when none applies.
function selectLine(lines, measure, sample) {
  const applying = lines.filter(line => line.measure === measure && holds(line.while ?? {}, sample.state));
  if (applying.length === 0) return null;
  const size = line => Object.keys(line.while ?? {}).length;
  const most = Math.max(...applying.map(size));
  const best = applying.filter(line => size(line) === most);
  return best.length > 1 ? { tie: best } : { line: best[0] };
}

/**
 * rows: the rows of a recording. known: the entries of known-failures.jsonc.
 * Returns { result: 'pass' | 'fail' | 'undecided', outcome: the result, or
 * 'known faults' for a failed run whose every line an entry with "fails"
 * names, unnamed: the lines no entry names, faultIssues, failures, undecided,
 * known, notOccurred, host, packetsLost }. A breach is reported once, at the sample in which it
 * passes its rule's "failAfterSeconds". known has the breaches that entries
 * cover, each with its entries. notOccurred has the entries whose state came
 * up without the breach; they do not change the result. host is the host
 * that the recording's first row names, or undefined: an entry with "host"
 * is judged only for a recording of that host. packetsLost has the seconds
 * in which the browser counted video packets as lost, and each failure has
 * those of its own seconds and of the 5 seconds before and after them.
 * changes has the row of each change with the settling time that the judge
 * found in the samples (settling()), in place of the walk's.
 *
 * again: true when the judge judges a saved recording again, with rules
 * that can differ from the walk's. A change that the recording cannot judge
 * under them is then in walkAgain, not in failures: one whose state the walk
 * found settled and the judge finds not settled before the next change, and
 * an unfinished one whose limit is now larger than the walk waited. outcome
 * is then 'walk again' in place of 'pass' or 'known faults'. noData has the
 * rules whose state came up in the recording and whose measure has no value
 * in any of its samples.
 */
export function judge(rows, criteria, allKnown = [], { again = false } = {}) {
  const { seconds, ...verdict } = examine(rows, criteria, allKnown, again);
  return verdict;
}

/**
 * For each sample of the rows, the measures that are not normal in its
 * second and those that are known to be normal:
 * [{ t, notNormal: [{ measure, value }], normal: [measure] }]. A measure is
 * not normal when its value has been outside the limit of its rule for as
 * many seconds in a row as the rule's "failAfterSeconds", or for one second
 * under a rule without it, and no known failure covers it: no entry
 * that applies to the sample, with "fails" or without, tolerates the value,
 * by the same matching as for a line of the report. It is normal when its value
 * is inside the limit or an entry covers it, and when it has no rule in
 * the sample's state. It is in neither list when it has a rule and no
 * value, when it is outside the limit for fewer seconds than that, and
 * while a change pauses it, which is until that change's switch
 * is done, or "pauseSeconds" after it: such a second does not keep a state from being settled. The
 * driver finds the settling time of a change in these seconds (settledAt()
 * in driver.mjs) during a walk, and the judge finds it in them again for
 * the whole recording (settling()).
 */
export function secondsNotNormal(rows, criteria, allKnown = []) {
  return examine(rows, criteria, allKnown).seconds;
}

function examine(rows, criteria, allKnown, again = false) {
  const host = rows.find(row => row.run)?.run.host ?? undefined;
  const known = expand(allKnown).filter(entry => entry.host === undefined || entry.host === host);
  // In the order in which they began: a change's row is written when its
  // state is settled, after the rows of the variables that it ended.
  const changes = rows.filter(row => row.change).sort((a, b) => a.startedAt - b.startedAt);
  for (const change of changes) {
    const missing = typeof change.startedAt !== 'number' ? 'startedAt'
      : !change.error && !change.unfinished && typeof change.switchSeconds !== 'number' ? 'switchSeconds'
        : change.endedBy && typeof change.seconds !== 'number' ? 'seconds' : null;
    if (missing) throw new Error(`the recording's row of the change ${changeName(change.change)} at ${change.t} has no "${missing}"`);
  }
  const samples = rows.filter(isSample);
  const lines = expand(criteria.everySecond ?? []);
  const failures = [];
  const undecided = [];
  const knownBreaches = [];
  const cameUp = new Set();   // entries whose situation was in the recording
  const seconds = [];         // per sample: the measures that are not normal in its second
  const occurred = new Set();  // entries with "fails" whose breach an entry without it covered
  const walkAgain = [];        // changes that the recording cannot judge under these rules
  const ruleCameUp = new Set(); // rules whose state was in the recording
  const hasValue = new Set();  // measures with a value in a sample of the recording

  const reportedMs = measure => reportsMs[measure] ?? 1000;
  const answeredAt = answerTimes(samples);
  // The state of a sample after the changes that began between its time and
  // the Mini's answer, or null without such a change.
  const stateAtAnswer = sample => {
    const began = changes.filter(change => change.startedAt > sample.t && change.startedAt <= answeredAt.get(sample));
    return began.length ? began.reduce((state, change) => ({ ...state, ...change.change }), sample.state) : null;
  };
  // Looked up once per change: the run judges its recording so far once per
  // second while it waits for a state to settle.
  const pauses = new Map(changes.map(change => {
    const rule = criteria.changeTakesAtMost?.find(line => sameChange(line.change, change.change));
    return [change, { measures: rule?.pauses ?? [], until: switchedAt(change) + (rule?.pauseSeconds ?? 0) * 1000 }];
  }));
  const pausesOf = change => pauses.get(change).measures;
  // The end of a change's pause: its switch is done, and the rule's pauseSeconds are over.
  const pauseEnd = change => pauses.get(change).until;
  // Whether an entry for a measure during a change applies to the sample: the
  // sample's second has a part of such a change's switch, in the state the
  // entry names, or of the entry's recoverySeconds after it.
  const duringItsChange = (entry, sample) => changes.some(change => sameChange(entry.change, change.change) &&
    holds(entry.while ?? {}, change.state ?? {}) && change.startedAt < sample.t &&
    sample.t - 1000 < switchedAt(change) + (entry.recoverySeconds ?? 0) * 1000);
  const ruleOf = {};      // measure -> the rule that judged it in the sample before
  const ruleSince = {};   // measure -> the time of the sample from which ruleOf has judged it
  const broken = {};      // measure -> consecutive samples outside the limit
  const reported = {};    // measure -> the current breach is in failures
  const covered = {};     // measure -> the current breach's row in knownBreaches
  const named = {};       // measure -> the current breach's row in failures, while an entry with "fails" names it
  // The rules and the entries of each measure, in their order in the files.
  const byMeasure = list => {
    const each = new Map();
    for (const one of list) each.set(one.measure, [...(each.get(one.measure) ?? []), one]);
    return measure => each.get(measure) ?? [];
  };
  const linesFor = byMeasure(lines);
  const knownFor = byMeasure(known);
  const lastHeld = new Map();   // entry -> the time of the last sample whose state it names
  const streak = new Map();     // entry -> consecutive samples of the breach in which it applied
  const endStreaks = measure => { for (const entry of knownFor(measure)) streak.delete(entry); };
  const endBreach = measure => {
    broken[measure] = 0;
    endStreaks(measure);
    reported[measure] = false;
    covered[measure] = null;
    named[measure] = null;
  };
  const tied = new Set();
  let previous = null;
  let lastDevice = null;
  const lastOf = {};      // measure -> the newest reports before this sample: { video, peer, device, counters, loads }
  // The states in which the rules want the Mini's report of a hold of the
  // sound, as during an upload on the Wi-Fi Mini.
  const holdExpected = sample => selectLine(lines, 'audioPausedByUpload', sample)?.line?.min >= 1;
  const soundHeldOff = soundHeldOffAt(samples, holdExpected);
  const restarting = restartingAt(samples);
  const memoryLost = memoryLostAt(samples);
  const tlsReserve = tlsReserveAt(samples);
  // No measure reports more than this before its sample.
  const longestReportMs = Math.max(1000, ...Object.values(reportsMs));
  const mqtt = mqttAt(samples);
  const sessionGone = sessionGoneAt(samples);
  const decodedShare = decodedShareAt(samples);
  const keyframes = keyframesAt(samples);
  const hostInput = hostInputAt(samples);
  // The single times of each kind, by sample (TIMED), with null for one
  // without an answer.
  const timesOf = { key: keyTimesAt(samples), line: lineTimesAt(samples, restarting) };
  // A 95th percentile: the times of its kind of the samples in the window of
  // the sample's rule, without those of a sample in which a change paused
  // the measure, and with the time to give up for one without an answer.
  // It has a value once the window is full: from the sample on whose rule
  // has applied to as many samples in a row without a pause, this one too,
  // as the window has seconds. windowOf has each such measure's samples
  // { t, times, line } of the longest window, ruleFrom the rule that
  // applied to its sample before and the count of the samples without a
  // pause since that rule began or since the last paused sample, and
  // windowAt, for the time of each sample with a value, the times that gave
  // it, with null for one without an answer.
  const windowOf = new Map(PERCENTILES.map(measure => [measure, []]));
  const ruleFrom = new Map();   // measure -> { line, samples }
  const windowAt = new Map(PERCENTILES.map(measure => [measure, new Map()]));
  const longestWindowMs = Math.max(0, ...lines.filter(line => line.windowSeconds).map(line => line.windowSeconds * 1000));
  const answerOf = measure => KINDS[TIMED[measure]].answerMs;
  const timesFor = (measure, sample) => timesOf[TIMED[measure]].get(sample).map(ms => ms ?? answerOf(measure));
  const percentileOf = (measure, sample, paused, line) => {
    const window = windowOf.get(measure);
    window.push({ t: sample.t, times: paused ? [] : timesOf[TIMED[measure]].get(sample), line });
    while (window.length && window[0].t <= sample.t - longestWindowMs) window.shift();
    const run = ruleFrom.get(measure);
    const count = paused ? 0 : run && run.line === line ? run.samples + 1 : 1;
    ruleFrom.set(measure, { line, samples: count });
    if (!line || count < line.windowSeconds) return undefined;
    const times = window.filter(one => one.line === line && one.t > sample.t - line.windowSeconds * 1000).flatMap(one => one.times);
    if (!times.length) return undefined;
    windowAt.get(measure).set(sample.t, times);
    return percentile95(times.map(ms => ms ?? answerOf(measure)));
  };
  for (const sample of samples) {
    const notNormal = [];
    const normal = [];
    seconds.push({ t: sample.t, notNormal, normal });
    // The changes whose switch can pause a measure in this sample.
    const switching = changes.filter(change => change.startedAt < sample.t && sample.t - longestReportMs < pauseEnd(change));
    for (const [measure, f] of Object.entries(measures)) {
      const last = lastOf[measure] ??= { video: undefined, device: undefined, counters: undefined, loads: {} };
      // A switch that is off has no report to compare the next one with.
      for (const name of Object.keys(last.loads)) if (!sample.state[name]) delete last.loads[name];
      const selected = selectLine(linesFor(measure), measure, sample);
      const after = conditionAtAnswer.includes(measure) ? stateAtAnswer(sample) : null;
      const otherRuleAtAnswer = after !== null && selected?.line !== selectLine(linesFor(measure), measure, { state: after })?.line;
      // A sample reports the second that ends at its time, or the longer
      // time of reportedMs. It is paused when any part of that time is
      // inside the change's pause.
      const paused = switching.some(change => pausesOf(change).includes(measure) &&
        sample.t - reportedMs(measure) < pauseEnd(change));
      const times = measure in TIMED ? timesFor(measure, sample) : undefined;
      const p95 = PERCENTILES.includes(measure) ? percentileOf(measure, sample, paused, selected?.line) : undefined;
      const value = otherRuleAtAnswer ? undefined : f(sample, previous, lastDevice, last, soundHeldOff.has(sample), restarting.has(sample), memoryLost.get(sample), tlsReserve.get(sample), mqtt.get(sample), sessionGone.get(sample), decodedShare.get(sample), keyframes.get(sample), times, p95, hostInput.get(sample));
      if (value !== undefined) hasValue.add(measure);
      for (const line of linesFor(measure)) if (holds(line.while ?? {}, sample.state)) ruleCameUp.add(line);
      // A report that is missing during this measure's pause would carry
      // what happened in the pause into the next report, so the next one is
      // not compared.
      if (paused) {
        if (!sample.browser?.video) last.video = last.peer = undefined;
        if (!sample.device) last.device = undefined;
        if (!sample.counters) last.counters = undefined;
        for (const name of Object.keys(last.loads)) if (!sample.loads?.[name]) delete last.loads[name];
      }
      if (sample.browser?.video) {
        last.video = sample.browser.video;
        last.peer = sample.browser.peer;
      }
      if (sample.device) last.device = sample.device;
      if (sample.counters) last.counters = sample.counters;
      for (const [name, load] of Object.entries(sample.loads ?? {})) if (load) last.loads[name] = load;
      // A breach ends when its rule no longer applies, also while the
      // measure has no value or is paused. A missing value of a measure from
      // the Mini's answer ends it as well when the Mini answered in the
      // sample (see fromAnswer); any other missing value leaves the breach
      // as it is.
      if (selected?.line !== ruleOf[measure]) {
        if (measure in ruleOf) ruleSince[measure] = sample.t;
        ruleOf[measure] = selected?.line;
        endBreach(measure);
      }
      if (paused) {
        broken[measure] = 0;
        endStreaks(measure);
        continue;
      }
      // The entries for this measure whose state is on, or ended no longer
      // ago than their recoverySeconds, and those whose change is under way
      // or finished no longer ago than that. The state counts also while the
      // measure has no value.
      const entries = knownFor(measure);
      for (const entry of entries) {
        if (entry.change ? !duringItsChange(entry, sample) : !holds(entry.while, sample.state)) continue;
        if (!entry.change) lastHeld.set(entry, sample.t);
        cameUp.add(entry);
      }
      // A measure without a rule in this state is normal in it; one with a
      // rule and no value is neither normal nor not.
      if (!selected) normal.push(measure);
      if (value === undefined && selected && sample.device && fromAnswer.has(measure)) endBreach(measure);
      if (value === undefined || !selected) continue;
      // A measure over a window of earlier samples is judged by a rule only
      // when the whole window lies in states of that rule: the window after
      // a state that the rule leaves out still holds that state's seconds.
      if (windowed.includes(measure) && sample.t - reportedMs(measure) < (ruleSince[measure] ?? -Infinity)) continue;
      if (selected.tie) {
        const key = JSON.stringify(selected.tie);
        if (!tied.has(key)) undecided.push({ t: sample.t, measure, state: sample.state, lines: selected.tie });
        tied.add(key);
        continue;
      }
      const { line } = selected;
      const applying = entries.filter(entry => (entry.change ? duringItsChange(entry, sample) : lastHeld.has(entry) &&
        sample.t - lastHeld.get(entry) <= (entry.recoverySeconds ?? 0) * 1000));
      if (withinLimit(line, value)) {
        normal.push(measure);
        endBreach(measure);
        continue;
      }
      broken[measure] = (broken[measure] ?? 0) + 1;
      // An entry counts the seconds of the breach in which it applies, so a
      // short known failure can lie inside a long one.
      for (const entry of entries) streak.set(entry, applying.includes(entry) ? (streak.get(entry) ?? 0) + 1 : 0);
      const tolerating = applying.filter(entry => streak.get(entry) <= entry.tolerateSeconds && withinLimit(entry, value));
      // Outside the rule's limit for as many seconds in a row as the rule's
      // "failAfterSeconds", or for one without it, and no entry tolerates
      // it, with "fails" or without: the second is not normal. A fault that
      // an entry with "fails" names has its own line in the report, and no
      // state waits for it. The seconds before that
      // are in neither list: they do not keep a state from being settled,
      // and they do not show that a measure from before a change is back.
      if (tolerating.length) normal.push(measure);
      else if (broken[measure] >= Math.max(1, line.failAfterSeconds ?? 0)) notNormal.push({ measure, value });
      if (broken[measure] <= (line.failAfterSeconds ?? 0)) continue;
      const faults = tolerating.filter(entry => entry.fails);
      if (reported[measure]) {
        // A failure that an entry with "fails" named goes on: it keeps the
        // name while such an entry tolerates it, and loses it for good in
        // the first second in which none does. An entry that tolerates a
        // later second of it, as after a change of the state, names it too.
        const failure = named[measure];
        if (failure && faults.length === 0) {
          failure.worseThan = [...new Set([...failure.worseThan, ...failure.faults])];
          failure.faults = [];
          named[measure] = null;
        } else if (failure) {
          failure.faults = [...new Set([...failure.faults, ...faults])];
          failure.worseThan = failure.worseThan.filter(entry => !faults.includes(entry));
        }
        continue;
      }
      const lastChange = changes.filter(change => change.startedAt <= sample.t).at(-1) ?? null;
      // An entry without "fails" that tolerates the breach makes it known,
      // also when an entry with "fails" names it.
      const covering = tolerating.filter(entry => !entry.fails);
      if (covering.length) {
        if (!covered[measure]) {
          covered[measure] = { t: sample.t, entries: [], measure, value, state: sample.state, lastChange };
          knownBreaches.push(covered[measure]);
        }
        covered[measure].seconds = broken[measure];
        for (const entry of covering) if (!covered[measure].entries.includes(entry)) covered[measure].entries.push(entry);
        faults.forEach(entry => occurred.add(entry));
        continue;
      }
      reported[measure] = true;
      const failure = {
        t: sample.t, measure, value, line, state: sample.state, lastChange, faults,
        worseThan: [...new Set([...(covered[measure]?.entries ?? []), ...applying])].filter(entry => !faults.includes(entry)),
        from: sample.t - (broken[measure] - 1) * 1000,
      };
      failures.push(failure);
      named[measure] = faults.length ? failure : null;
      if (covered[measure]) knownBreaches.splice(knownBreaches.indexOf(covered[measure]), 1);
    }
    previous = sample;
    if (sample.device) lastDevice = sample.device;
  }

  // The changes, judged by their settling times in the seconds above with
  // these rules (settling() below), not by the times that the walk found.
  // judgedChanges has the row of each change whose state the judge found
  // settled or not, with the judge's seconds, waitedFor and unsettled.
  const judgedChanges = [];
  const changeKnown = [];
  for (const [index, change] of changes.entries()) {
    const max = criteria.changeTakesAtMost?.find(line => sameChange(line.change, change.change))?.max;
    if (change.error) {
      failures.push({ t: change.t, change, text: change.error, from: change.startedAt });
      continue;
    }
    const entries = known.filter(k => k.change && k.measure === undefined && sameChange(k.change, change.change) && holds(k.while ?? {}, change.state ?? {}));
    entries.forEach(entry => cameUp.add(entry));
    // The driver gave the switch its limit, or its known failure's, and
    // more: no entry covers a switch that was not done then. A limit that
    // is now larger than the walk waited asks for a new walk.
    if (change.unfinished) {
      const limits = [max, ...entries.map(entry => entry.max)].filter(limit => typeof limit === 'number');
      const limit = limits.length ? Math.max(...limits) : undefined;
      if (again && limit > change.seconds) {
        walkAgain.push({ t: change.t, change, limit, movedOn: change.startedAt + change.seconds * 1000 });
        continue;
      }
      failures.push({ t: change.t, change, max, value: change.seconds, unfinished: change.unfinished, worseThan: entries, from: change.startedAt });
      continue;
    }
    // A variable that another change ended has the time of its own switch,
    // and a change whose ended variable failed has that failure.
    const settled = change.endedBy ? { seconds: change.seconds } : settling(changes, index, seconds);
    if (!settled) continue;
    const { unsettled, waitedFor, ...recorded } = change;
    const judged = { ...recorded, ...settled };
    judgedChanges.push(judged);
    // A state that the walk found settled, and that these rules find not
    // settled before the next change, had no more time in the walk: a new
    // walk with these rules waits for it.
    if (again && settled.unsettled && settled.settleEnd === 'before the next change' && !unsettled) {
      walkAgain.push({ t: change.t, change: judged, movedOn: settled.movedOn });
      continue;
    }
    // Nor does an entry cover a state that was not settled before the next
    // change, or when the driver's wait for that was over.
    if (settled.unsettled) {
      failures.push({ t: change.t, change: judged, max, value: settled.seconds, unsettled: settled.unsettled,
        settleEnd: settled.settleEnd, worseThan: entries, from: change.startedAt });
      continue;
    }
    if (max === undefined || settled.seconds <= max) continue;
    const tolerating = entries.filter(entry => settled.seconds <= entry.max);
    const covering = tolerating.filter(entry => !entry.fails);
    if (covering.length) {
      changeKnown.push({ t: change.t, entries: covering, change: judged, max, seconds: settled.seconds });
      tolerating.forEach(entry => occurred.add(entry));
    } else {
      failures.push({ t: change.t, change: judged, max, value: settled.seconds, faults: tolerating,
        worseThan: entries.filter(entry => !tolerating.includes(entry)), from: change.startedAt });
    }
  }
  knownBreaches.unshift(...changeKnown);

  // Beside a breach of a measure of single times (TIMED): the times of its
  // kind in the sample that the line reports, or for a 95th percentile in
  // that sample's window, with how many were over the rule's limit, how
  // many had no answer, and the slowest. No rule or entry reads them.
  const timedDetail = (measure, t, line) => {
    const kind = TIMED[measure];
    const raw = PERCENTILES.includes(measure) ? windowAt.get(measure).get(t) ?? []
      : samples.filter(sample => sample.t === t).flatMap(sample => timesOf[kind].get(sample));
    if (!raw.length) return undefined;
    const { one, many, answerMs } = KINDS[kind];
    const times = raw.map(ms => ms ?? answerMs);
    const over = times.filter(ms => ms > line.max).length;
    const unanswered = raw.filter(ms => ms === null).length;
    return `${PERCENTILES.includes(measure) ? `the last ${line.windowSeconds} s: ` : ''}${raw.length} ${raw.length === 1 ? one : many}, ` +
      `${over} over ${line.max} ms, ${unanswered} without an answer within ${answerMs / 1000} s, slowest ${Number(Math.max(...times).toFixed(1))} ms`;
  };
  for (const row of [...failures, ...knownBreaches]) {
    if (!(row.measure in TIMED)) continue;
    const line = row.line ?? selectLine(lines, row.measure, { state: row.state })?.line;
    if (line) row.detail = timedDetail(row.measure, row.t, line);
  }

  // The sender's log of the list (run.mjs): a line that says that the Mini
  // sent something that the OS Services protocol does not allow fails the
  // run, and so does a log that could not be read. No entry covers either.
  for (const row of rows.filter(row => row.senderLog)) {
    const { protocolErrors = [], error } = row.senderLog;
    if (error || protocolErrors.length) failures.push({ t: row.t, from: row.t, senderLog: row.senderLog });
  }
  failures.sort((a, b) => a.t - b.t);
  const seen = new Set([...occurred, ...knownBreaches.flatMap(k => k.entries), ...failures.flatMap(f => [...(f.worseThan ?? []), ...(f.faults ?? [])])]);
  const notOccurred = [...cameUp].filter(entry => !seen.has(entry));
  const result = failures.length ? 'fail' : undecided.length ? 'undecided' : 'pass';
  // The three ways a run ends: 'pass', 'known faults' when every failure has
  // an entry with "fails" that names it, and 'fail' when a line has none. An
  // undecided sample beside the failures is such a line.
  const unnamed = failures.filter(failure => !failure.faults?.length).length + (failures.length ? undecided.length : 0);
  // A change that the recording cannot judge leaves 'pass' and 'known
  // faults' open: 'walk again'.
  const ended = result === 'fail' && unnamed === 0 ? 'known faults' : result;
  const outcome = walkAgain.length && ['pass', 'known faults'].includes(ended) ? 'walk again' : ended;
  const faultIssues = [...new Set(failures.flatMap(failure => (failure.faults ?? []).map(issueNumber)))].sort((a, b) => a - b);
  // The 1-minute load of the machine that ran the tool, which each sample
  // has: the highest of the run, and for each failure the highest in the
  // seconds of its breach or its change. No rule or entry reads it.
  const highest = list => (list.length ? Math.max(...list) : undefined);
  const loads = (from, to) => samples.filter(sample => typeof sample.load === 'number' && sample.t >= from && sample.t <= to)
    .map(sample => sample.load);
  for (const failure of failures) failure.load = highest(loads(failure.from, failure.t));
  // The seconds with lost video packets, and beside each failure those
  // near its breach or its change.
  const packetsLost = lostPackets(samples);
  for (const failure of failures) {
    failure.packetsLost = packetsLost.filter(lost => lost.t >= failure.from - LOSS_SHOWS_FOR_MS && lost.t <= failure.t + LOSS_SHOWS_FOR_MS);
  }
  const load = { highest: highest(loads(-Infinity, Infinity)), cores: rows.find(row => row.run)?.run.cores };
  // The seconds in which the machine that ran the tool was off the network,
  // and beside each failure those of its breach or its change and of the
  // LINK_SHOWS_FOR_MS before it. No rule or entry reads them.
  const link = { interface: rows.find(row => row.run)?.run.toolInterface, off: offNetwork(samples) };
  for (const failure of failures) {
    failure.offNetwork = link.off.filter(t => t >= failure.from - LINK_SHOWS_FOR_MS && t <= failure.t);
  }
  // The Mini's stored settings as the run found them, from the first row.
  const settings = rows.find(row => row.run)?.run.settings;
  const noData = lines.filter(line => ruleCameUp.has(line) && !hasValue.has(line.measure));
  return { result, outcome, unnamed, faultIssues, failures, undecided, known: knownBreaches, notOccurred, host, load, link, settings, packetsLost,
    walkAgain, noData, changes: judgedChanges, seconds };
}

/**
 * The settling of the change changes[index] in seconds, the result of
 * secondsNotNormal() for the same rows, as the driver finds it during a
 * walk (settledAt() in driver.mjs). The state can settle from the time at
 * which the change's switch is done, or from the time at which the last
 * variable that the change ended is off (`endedBy`). The samples that
 * count are those up to the start of the next change, and up to
 * SETTLE_CAP_SECONDS after that time, as the driver waits no longer.
 *
 * Returns { seconds, waitedFor } for a state that settled: the time from the
 * start of the change to the start of the normal seconds, and the measures
 * that were not normal before (settledAt()). A state that did not settle
 * also has `unsettled`, the measures that were not normal last, and
 * `settleEnd`, which tells where the samples that count end; its seconds
 * run until there. movedOn is the start of the next change, when that is
 * where they end. Returns null for a change whose ended variable has an
 * error: that row is the failure.
 */
function settling(changes, index, seconds) {
  const change = changes[index];
  const name = Object.keys(change.change)[0];
  let next = index + 1;
  while (changes[next]?.endedBy === name) next++;
  const ended = changes.slice(index + 1, next);
  if (ended.some(row => row.error)) return null;
  const from = Math.max(switchedAt(change), ...ended.map(row => row.startedAt + row.seconds * 1000));
  const capAt = from + SETTLE_CAP_SECONDS * 1000;
  const nextAt = changes[next]?.startedAt ?? Infinity;
  const end = Math.min(capAt, nextAt, seconds.at(-1)?.t ?? from);
  const seen = settledAt(seconds.filter(second => second.t <= end), change.startedAt, from);
  if (seen.settledAt !== null) return { seconds: (seen.settledAt - change.startedAt) / 1000, waitedFor: seen.waitedFor };
  return {
    seconds: (end - change.startedAt) / 1000, waitedFor: seen.waitedFor,
    unsettled: seen.last ?? 'the recording has no seconds after the switch',
    settleEnd: end === capAt ? `in ${SETTLE_CAP_SECONDS} s` : end === nextAt ? 'before the next change' : 'before the recording ended',
    ...(end === nextAt && end !== capAt ? { movedOn: nextAt } : {}),
  };
}

// A session needs this long to be back after the tool machine's network is:
// the browser's connection, the cloud's socket, the first answers.
const LINK_SHOWS_FOR_MS = 10_000;

// The times of the samples in which the machine that ran the tool was off
// the network: its interface towards the Mini was without its link or its
// address, or the link came or went since the sample before. In those seconds
// no request reaches the Mini and no packet reaches the browser, so the
// recording says nothing about the Mini.
function offNetwork(samples) {
  const off = [];
  // The newest count of link changes that a sample had: a sample without
  // the count, as while the interface is gone, keeps the one before it.
  let count;
  for (const sample of samples) {
    const link = sample.link;
    if (!link) continue;
    const changed = count !== undefined && link.carrierChanges > count;
    if (link.carrier === false || link.address === false || changed) off.push(sample.t);
    if (link.carrierChanges !== undefined) count = link.carrierChanges;
  }
  return off;
}

/**
 * The version of the tool in the git checkout at root: { commit, changed },
 * the commit of HEAD and whether tools/combined_load has changes that are not
 * committed, files that git does not track among them; { error } when git
 * cannot tell. The walk writes it into the recording's first row as `tool`.
 */
export function toolVersion(root) {
  const git = (...args) => execFileSync('git', ['-C', root, ...args], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }).trim();
  try {
    return { commit: git('rev-parse', 'HEAD'), changed: git('status', '--porcelain', '--', 'tools/combined_load') !== '' };
  } catch (e) {
    return { error: e.message.split('\n')[0] };
  }
}

/**
 * The NOTE lines of a judge that judges a saved recording: recorded is the
 * `tool` of the recording's first row, current the toolVersion() of the
 * checkout that judges it. None when both name the same commit without
 * changes in tools/combined_load.
 */
export function toolVersionNotes(recorded, current) {
  if (!recorded) return ['the recording does not name its tool: its first row has no "tool"'];
  const name = version => (version.commit === undefined ? `an unknown commit (${version.error})`
    : `${version.commit}${version.changed ? ' with changes in tools/combined_load that are not committed' : ''}`);
  const same = recorded.commit !== undefined && recorded.commit === current.commit && !recorded.changed && !current.changed;
  return same ? [] : [`the recording was made by the tool at ${name(recorded)}, and the judge runs at ${name(current)}`];
}

// The number of an entry's issue, from its URL.
const issueNumber = entry => Number(entry.issue.match(/(\d+)\/?$/)?.[1]);

const describeState = state => Object.entries(state).filter(([, value]) => value)
  .map(([name, value]) => (value === true ? name : `${name}=${value}`)).join(' + ') || 'idle';

/** The result of judge() as text. startedAt: the time of the recording's first row. */
export function report({ result, outcome = result, unnamed, faultIssues = [], failures, undecided, known = [], notOccurred = [], host, load = {}, link = {}, settings, packetsLost = [], walkAgain = [], noData = [], toolNotes = [] }, startedAt) {
  const at = t => `${((t - startedAt) / 1000).toFixed(0)} s`;
  const out = [host === undefined
    ? 'HOST  none in the recording: no known failure with "host" applies'
    : `HOST  ${host}`];
  if (load.highest !== undefined) {
    out.push(`LOAD  ${load.highest} at most, the 1-minute load of the machine that ran the tool${load.cores ? `, which has ${load.cores} cores` : ''}`);
  }
  // The seconds in which the tool machine was off the network, as spans of
  // samples that follow each other.
  const spans = times => times.reduce((list, t) => {
    const last = list.at(-1);
    if (last && t - last[1] <= 1500) last[1] = t;
    else list.push([t, t]);
    return list;
  }, []).map(([from, to]) => (from === to ? at(from) : `${at(from).replace(' s', '')} to ${at(to)}`)).join(', ');
  if (link.off?.length) {
    out.push(`LINK  the machine that ran the tool was off the network at ${spans(link.off)}` +
      `${link.interface ? `: its interface ${link.interface} lost its link or its address` : ''}`);
  }
  const offNetwork = f => (f.offNetwork?.length ? [`tool machine off the network: ${spans(f.offNetwork)}`] : []);
  // A stored setting of the Mini that was not at its default when the run
  // began: the bench was not as a user gets the Mini.
  for (const line of settingLines(settings)) out.push(`SETTING  ${line}`);
  // The seconds with lost video packets. No rule of the Wi-Fi Mini reads
  // them, and a session recovers from them; a failure in the seconds after
  // them has them beside it.
  const lostAt = lost => lost.map(({ t, packets }) => `${packets} at ${at(t)}`).join(', ');
  if (packetsLost.length) out.push(`LOST  video packets: ${lostAt(packetsLost)}`);
  const lostNear = f => (f.packetsLost?.length ? [`video packets lost: ${lostAt(f.packetsLost)}`] : []);
  // A machine with a load above half its cores is busy enough to cause a
  // breach by itself: its browser decodes the video and sends the keys.
  const busy = f => (load.cores && f.load > load.cores / 2 ? [`tool machine busy: load ${f.load} on ${load.cores} cores`] : []);
  const lastChange = f => (f.lastChange
    ? [`last change: ${changeName(f.lastChange.change)}, started ${((f.t - f.lastChange.startedAt) / 1000).toFixed(0)} s earlier`] : []);
  const add = (label, first, ...more) => {
    out.push(`${label}  ${first}`, ...more.map(line => `${' '.repeat(label.length)}  ${line}`));
  };
  const issues = k => [...new Set(k.entries.map(entry => entry.issue))];
  // Beside the line of a failure that an entry with "fails" names.
  const fault = f => (f.faults?.length ? `    known fault: ${[...new Set(f.faults.map(issueNumber))].map(n => `#${n}`).join(', ')}` : '');
  // What the state waited for after the change's switch was done: each
  // measure that was not normal, for how many seconds, and its last such value.
  const waitedFor = change => {
    const each = Object.entries(change.waitedFor ?? {})
      .map(([measure, { seconds, last }]) => `${measure} for ${seconds} s, last ${Number(last.toFixed(1))}`);
    return each.length ? [`not normal after the switch, which was done after ${change.switchSeconds.toFixed(1)} s: ${each.join('; ')}`] : [];
  };
  // The times beside a breach of a measure of single times.
  const detail = f => (f.detail ? ` (${f.detail})` : '');
  // A variable that another change ended has the time of its own switch.
  const took = (change, seconds) => (change.endedBy
    ? `ended by ${change.endedBy} on, off after ${seconds.toFixed(1)} s` : `settled after ${seconds.toFixed(1)} s`);
  for (const f of failures) {
    const worse = (f.worseThan ?? []).map(entry => `worse than the known failure ${entry.issue}: ${JSON.stringify(entry)}`);
    if (f.senderLog) {
      const { error, protocolErrors = [] } = f.senderLog;
      add('FAIL', error ? `the sender's log could not be read: ${error}`
        : `the sender's log has ${protocolErrors.length} ${protocolErrors.length === 1 ? 'line' : 'lines'} of a protocol error`,
      ...protocolErrors.slice(0, 5), ...(protocolErrors.length > 5 ? [`and ${protocolErrors.length - 5} more`] : []));
    } else if (f.change) {
      const limit = f.max === undefined ? '' : `, limit ${f.max} s`;
      add('FAIL', `${changeName(f.change.change)} ` +
        (f.text ? `failed: ${f.text}`
          : f.unfinished ? `was not finished after ${f.value.toFixed(1)} s${limit}: ${f.unfinished}`
            : f.unsettled ? `did not settle ${f.settleEnd}${limit}: ${f.unsettled}`
              : `${took(f.change, f.value)}${limit}`) + fault(f),
      `at ${at(f.t)} in state ${describeState(f.change.state ?? {})}`, ...waitedFor(f.change), ...lostNear(f), ...busy(f), ...offNetwork(f), ...worse);
    } else {
      add('FAIL', `${f.measure} is ${Number(f.value.toFixed(1))} for more than ${f.line.failAfterSeconds ?? 0} s${detail(f)}` + fault(f),
        `at ${at(f.t)} in state ${describeState(f.state)}`, ...lastChange(f), ...lostNear(f), ...busy(f), ...offNetwork(f), `rule: ${JSON.stringify(f.line)}`, ...worse);
    }
  }
  for (const k of known) {
    if (k.change) {
      add('KNOWN', `${changeName(k.change.change)} ${took(k.change, k.seconds)}, limit ${k.max} s`,
        `at ${at(k.t)} in state ${describeState(k.change.state ?? {})}`, ...waitedFor(k.change), ...issues(k));
    } else {
      add('KNOWN', `${k.measure} is ${Number(k.value.toFixed(1))} for ${k.seconds} s${detail(k)}`,
        `at ${at(k.t)} in state ${describeState(k.state)}`, ...lastChange(k), ...issues(k));
    }
  }
  for (const w of walkAgain) {
    add('WALK AGAIN', `${changeName(w.change.change)} ` + (w.limit === undefined
      ? 'did not settle before the next change under the current rules'
      : `was not finished after ${w.change.seconds.toFixed(1)} s, and the current limit is ${w.limit} s`) +
      `; the walk moved on at ${at(w.movedOn)}`, `at ${at(w.t)} in state ${describeState(w.change.state ?? {})}`);
  }
  for (const line of toolNotes) add('NOTE', line);
  for (const entry of notOccurred) {
    add('NOTE', 'a known failure did not occur in this run', entry.issue, JSON.stringify(entry));
  }
  for (const line of noData) {
    add('NOTE', `the recording has no data for the rule of ${line.measure}`, JSON.stringify(line));
  }
  for (const u of undecided) {
    add('UNDECIDED', `${u.measure} in state ${describeState(u.state)} at ${at(u.t)}: these rules apply equally;`,
      'add a rule that names the variables of both', ...u.lines.map(line => JSON.stringify(line)));
  }
  // The last line: PASS; WALK AGAIN with the number of changes that the
  // recording cannot judge; FAIL with known faults only; or FAIL with the
  // number of lines that no entry names.
  if (outcome === 'walk again') {
    out.push(`WALK AGAIN  ${walkAgain.length} ${walkAgain.length === 1 ? 'change' : 'changes'} that the recording cannot judge` +
      `${faultIssues.length ? `, and known faults: ${faultIssues.map(n => `#${n}`).join(', ')}` : ''}`);
  } else if (outcome === 'known faults') out.push(`FAIL  known faults only: ${faultIssues.map(n => `#${n}`).join(', ')}`);
  else if (result === 'fail' && unnamed !== undefined) {
    out.push(`FAIL  ${unnamed} ${unnamed === 1 ? 'line' : 'lines'} that no entry names${faultIssues.length ? `, and known faults: ${faultIssues.map(n => `#${n}`).join(', ')}` : ''}`);
  } else out.push(result.toUpperCase());
  return out.join('\n');
}

/**
 * Reads a file the tool's users edit: JSON in which a line that starts with
 * "//" is a comment. A comment at the end of a line is not allowed, so a
 * "//" inside a value, as in a URL, needs no care.
 */
export const readJsonc = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\s*\/\/.*$/gm, ''));

export const readRows = file => fs.readFileSync(file, 'utf8').split('\n').filter(Boolean).map(line => JSON.parse(line));

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [recording, ...files] = process.argv.slice(2);
  if (!recording) {
    console.error('usage: judge.mjs <recording.jsonl> [criteria.jsonc] [known-failures.jsonc]');
    process.exit(2);
  }
  const rows = readRows(recording);
  // A recording without a board is an Ethernet one.
  const directory = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', rows[0]?.run?.board ?? 'ethernet');
  const [table = path.join(directory, 'criteria.jsonc'), knownFile = path.join(directory, 'known-failures.jsonc')] = files;
  const criteria = readJsonc(table);
  const known = readJsonc(knownFile);
  // switches.mjs loads the tool's switches, so only the CLI imports it. It
  // imports this file, so the import waits until this file has run: an
  // await here would wait for itself.
  import('./switches.mjs').then(({ VARIABLES }) => {
    for (const [file, validate, content] of [[table, validateCriteria, criteria], [knownFile, validateKnown, known]]) {
      try {
        validate(content, VARIABLES);
      } catch (e) {
        console.error(`${file}: ${e.message}`);
        process.exit(2);
      }
    }
    const verdict = judge(rows, criteria, known, { again: true });
    const checkout = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
    const toolNotes = toolVersionNotes(rows[0]?.run?.tool, toolVersion(checkout));
    console.log(report({ ...verdict, toolNotes }, rows[0]?.t ?? 0));
    process.exit(EXIT[verdict.outcome] ?? 1);
  });
}
