// Writes the recording of a run, one JSON object per line. Once per second a
// sample row:
//
//   { t, state, device, counters, browser, loads, load, link }
//
// device: /webrtc/stats?bitrate=1, or null. counters: the full stats' extras
// (memoryCounters), when fetched (fullStatsPause), else null. browser: the
// page's inbound video report and presented frame times, or null without a
// peer connection. loads: each active switch's sample(), stamped `at`.
// load: the tool machine's 1-minute load average. link: its interface
// towards the Mini (toolLink).
//
// All sources are asked at once with a timeout so rows stay one second
// apart; a late switch sample is folded into its next one (withLate). The
// driver's change rows go to the same file through record().
import dgram from 'node:dgram';
import dns from 'node:dns';
import fs from 'node:fs';
import os from 'node:os';
import { sleep } from './driver.mjs';

const SAMPLE_MS = 1000;
// A source that does not answer in this time is recorded as null for the sample.
const SOURCE_TIMEOUT_MS = 900;

// The Mini's counters of allocations that failed, by their place in the full
// /webrtc/stats. Each counts since the Mini's start. A counter that the
// Mini's build or board does not have is left out.
export const ALLOCATION_FAILURES = ['memoryBudget.controlAllocationFailures', 'memoryBudget.jsonAllocationFailures',
  'memoryBudget.lvglAllocationFailures', 'peerMemoryArena.allocationFailures', 'peerMemoryArena.beginFailures',
  'usbHid.uploadWrite.stagingAllocationFailures', 'ethernetRx.pbufAllocationFailures'];

/**
 * What a sample keeps of the full /webrtc/stats, which is some tens of
 * kilobytes: the Mini's counts of what failed or was refused, each since its
 * start, and the least free memory since its start.
 *
 *   allocationFailures       { <place in the answer>: count } (ALLOCATION_FAILURES)
 *   videoRebuildFailures     rebuilds of the video capture that failed
 *   videoRebuildBeginDenials tries to begin a rebuild that had to wait
 *   uploadWriteErrors        writes of an upload to the TF card that failed
 *   admissionDenials         memory requests that the budget refused, of every owner
 *   tlsAdmissions            { <owner>: { admitted, denied, minInternalHeadroom, minDmaHeadroom,
 *                            lastInternalHeadroom, lastDmaHeadroom, minInternalLargest, minDmaLargest,
 *                            reservedInternal, reservedDma, heldInternalMin, heldDmaMin,
 *                            heldInternalFallMax, heldDmaFallMax, heldOverruns, heldUnderFloor } }: the requests
 *                            for a TLS connection, or for a read on an open one, and the memory
 *                            above the floors at such a request, the least and that of the newest;
 *                            the smallest largest free block at a request; what the owner has reserved now;
 *                            and what the Mini saw while the owner held a reservation: the least
 *                            free memory, the largest fall from the free memory at an admission,
 *                            the count of holds whose fall was larger than what was reserved
 *                            during them and the room between the floors, and the count of holds
 *                            with free memory under an admission floor
 *   freeInternalLowWater, freeDmaLowWater, freePsramLowWater
 *                            the least free internal RAM, DMA-capable RAM and PSRAM, in bytes
 *   freeInternal             the free internal RAM at this answer, in bytes
 *
 * nowMs is the Mini's clock in the short answer of the same sample, by which
 * the judge sees a restart; answerMs is how long the full answer took. null
 * for an answer that is not the full one.
 */
export function memoryCounters(answer, { nowMs, answerMs } = {}) {
  const budget = answer?.memoryBudget;
  if (!budget) return null;
  const at = place => place.split('.').reduce((value, key) => value?.[key], answer);
  const number = value => (typeof value === 'number' ? value : undefined);
  return {
    ...(typeof nowMs === 'number' ? { nowMs } : {}),
    ...(typeof answerMs === 'number' ? { answerMs } : {}),
    allocationFailures: Object.fromEntries(ALLOCATION_FAILURES.map(place => [place, number(at(place))])
      .filter(([, count]) => count !== undefined)),
    videoRebuildFailures: number(budget.videoRebuildFailures),
    videoRebuildBeginDenials: number(budget.videoRebuildBeginDenials),
    uploadWriteErrors: number(answer.usbHid?.uploadWrite?.errors),
    admissionDenials: number(budget.admissionDenials),
    tlsAdmissions: Object.fromEntries((budget.tlsAdmissions ?? [])
      .map(({ owner, admitted, denied, minInternalHeadroom, minDmaHeadroom, lastInternalHeadroom, lastDmaHeadroom,
        minInternalLargest, minDmaLargest, reservedInternal, reservedDma, heldInternalMin, heldDmaMin,
        heldInternalFallMax, heldDmaFallMax, heldOverruns, heldUnderFloor }) =>
        [owner, { admitted, denied, minInternalHeadroom, minDmaHeadroom, lastInternalHeadroom, lastDmaHeadroom,
          minInternalLargest, minDmaLargest, reservedInternal, reservedDma, heldInternalMin, heldDmaMin,
          heldInternalFallMax, heldDmaFallMax, heldOverruns, heldUnderFloor }])),
    freeInternalLowWater: number(answer.internalHeapMinimum),
    freeDmaLowWater: number(answer.internalDmaHeapMinimum),
    freePsramLowWater: number(answer.psramMinimum),
    freeInternal: number(answer.internalHeapFree),
  };
}

// The Mini builds the full stats in about 150 ms of processor time, below
// the priority of its other work, in the one task that serves every HTTP
// request. Beside a host that reads a mount at full speed the answer takes
// 0.2 to 0.5 s and more, and a request that waits for it holds up the short
// answers behind it: asked for once per second, the short answer was missing
// for whole states. So the recorder asks for the full stats only after the
// short answer of the same sample has come, with what is left of the
// sample's time, and at most once in FULL_STATS_MS. The counts are the
// Mini's since its start, so nothing is lost between two answers.
const FULL_STATS_MS = 5000;
// An answer that took longer than this, or did not come, doubles the pause
// before the next request, up to the longest one.
const FULL_STATS_SLOW_MS = 500;
const FULL_STATS_LONGEST_MS = 40_000;
// The least time that must be left of a sample for the request to be made.
const FULL_STATS_NEEDS_MS = 400;
// While a host reads an image on the TF card, the Mini needs seconds for the
// full answer and gives no short answer in them, so several samples in a row
// can have no full stats. The run names those states (withoutFullStats), and the
// recorder asks for the full stats there only in the recording's last
// sample, for which it waits this long.
const FULL_STATS_LAST_MS = 5000;

/**
 * The pause before a request for the full stats in a state that restarts
 * the Mini (restarts), and in any other state: a restart sets the Mini's
 * counts to zero, so what they count before it is read at the shortest
 * pause, however slow the answers before were.
 */
export const pauseNow = (pauseMs, restarts) => (restarts ? FULL_STATS_MS : pauseMs);

/**
 * Whether a sample asks for the full stats again, sinceMs after the sample
 * that asked before, with the pause pauseMs. The samples begin 999 to
 * 1001 ms apart, so a sample that is due may begin a few ms before the end of
 * the pause; it asks all the same, and the answers stay pauseMs apart
 * instead of one sample more.
 */
export const fullStatsDue = (sinceMs, pauseMs) => sinceMs >= pauseMs - SAMPLE_MS / 2;

/**
 * The pause before the next request for the full stats. pauseMs: the pause
 * before this one. answerMs: how long the answer took, or null without one.
 */
export function fullStatsPause(pauseMs, answerMs) {
  return answerMs === null || answerMs > FULL_STATS_SLOW_MS ? Math.min(pauseMs * 2, FULL_STATS_LONGEST_MS) : FULL_STATS_MS;
}

// The JSON of the Mini's answer to a GET, which fails when it does not come in ms.
const getJson = (url, ms) => fetch(url, { signal: AbortSignal.timeout(ms) }).then(r => r.json());

// In the page: the time of each frame that the video element presented, in
// ms of the page's clock (performance.now()), since the newest call whose
// answer is in a sample: { before, times, now }. times has those frames,
// before is the newest frame before them, null before the element's first
// frame, and now is the page's clock at this call. null before the
// element's first frame. The browser calls onFrame once for each frame it
// presents, and the page keeps its time; a page whose answers reach no
// sample keeps the newest 600 frames.
// read: the number of this call. seen: the number of the newest call whose
// answer the recorder has. The frames of a call whose answer came too late
// for its sample are in the next answer as well.
export const frameTimes = (read, seen) => {
  const element = document.querySelector('video');
  if (!element?.requestVideoFrameCallback) return null;
  let watch = window.__combinedLoadFrames;
  if (watch?.element !== element) {
    watch = window.__combinedLoadFrames = { element, before: null, times: [], taken: 0, read: undefined };
    const state = watch;
    const onFrame = (now, frame) => {
      state.times.push(Math.round(frame.presentationTime * 10) / 10);
      if (state.times.length > 600) {
        state.before = state.times.shift();
        state.taken = Math.max(0, state.taken - 1);
      }
      element.requestVideoFrameCallback(onFrame);
    };
    element.requestVideoFrameCallback(onFrame);
  }
  // A call that a later one overtook is in no sample, and changes nothing.
  if (read < watch.read) return null;
  // The frames of the answer before are in a sample.
  if (seen === watch.read && watch.taken) {
    watch.before = watch.times[watch.taken - 1];
    watch.times.splice(0, watch.taken);
  }
  if (watch.before === null && !watch.times.length) return null;
  watch.taken = watch.times.length;
  watch.read = read;
  return { before: watch.before, times: [...watch.times], now: Math.round(performance.now() * 10) / 10 };
};

const inboundVideo = async (frames, read, seen) => {
  const pc = window.__kvmTestHooks?._getPeerConnection?.();
  if (!pc) return null;
  // peer: a name for each RTCPeerConnection that the page has had, so that
  // a new session of the page is told from a new stream in the same
  // session: the time at which this document was loaded, and the
  // connection's number among those that a sample saw in it.
  const peers = window.__combinedLoadPeers ??= { seen: new WeakMap(), count: 0 };
  if (!peers.seen.has(pc)) peers.seen.set(pc, `${Math.round(performance.timeOrigin)}.${++peers.count}`);
  let video = null;
  (await pc.getStats()).forEach(report => {
    if (report.type === 'inbound-rtp' && report.kind === 'video') video = { ...report };
  });
  return { connectionState: pc.connectionState, peer: peers.seen.get(pc), video, frames: frames(read, seen) };
};
// Both in one call of the page, with the number of the call and the number
// of the newest call whose answer is in a sample.
const browserReport = (read, seen) => `(${inboundVideo})(${frameTimes}, ${read}, ${seen})`;

/**
 * A switch's sample with the lists of its samples that came after their
 * time, late, in front of its own: each list of a switch's sample holds the
 * events since the sample before, so the events of a late sample belong to
 * the next one. The other fields are the newer sample's.
 */
export function withLate(late, sample) {
  if (!late || !sample) return sample;
  const joined = { ...sample };
  for (const [name, list] of Object.entries(late)) {
    if (Array.isArray(list) && Array.isArray(sample[name])) joined[name] = [...list, ...sample[name]];
  }
  return joined;
}

// The address of this machine from which the system's routes reach `host`,
// and its family (4 or 6): the local address of a UDP socket that is
// connected to it. Connecting a UDP socket sends no packet. `host` is a name
// or an address; an IPv6 address may be in brackets, as in a URL. null when
// the name does not resolve or there is no route.
export const routeAddress = async (host, port = 80) => {
  const target = await dns.promises.lookup(host.replace(/^\[|\]$/g, '')).catch(() => null);
  if (!target) return null;
  return new Promise(resolve => {
    const socket = dgram.createSocket(target.family === 6 ? 'udp6' : 'udp4');
    const done = address => {
      socket.close();
      resolve(address && { address, family: target.family });
    };
    socket.once('error', () => done(null));
    socket.connect(port, target.address, () => done(socket.address().address));
  });
};

// An address that a link gives itself, which reaches no Mini by a route.
const linkLocal = address => /^fe[89ab]/i.test(address) || address.startsWith('169.254.');

/**
 * The network interface of this machine that carries the route to the Mini
 * at `url`, and a reader of its state for each sample:
 *
 *   { name, read() -> { carrier, carrierChanges, address } }
 *
 * carrier is whether the interface has its link, and carrierChanges the
 * system's count of the times the link came or went, which also shows a link
 * that went and came back between two samples (both from /sys/class/net on
 * Linux; without a value on another system). address is whether the
 * interface has an address of the route's family that is not link-local: it
 * has none between a link that came back and the next DHCP lease, and a
 * lease with another address than before counts as an address. null when the
 * route's address belongs to no interface.
 */
export async function toolLink(url, { route = routeAddress, interfaces = os.networkInterfaces,
  readFile = file => fs.readFileSync(file, 'utf8') } = {}) {
  const { hostname, port, protocol } = new URL(url);
  const from = await route(hostname, Number(port) || (protocol === 'https:' ? 443 : 80));
  const name = from && Object.entries(interfaces()).find(([, entries]) => entries.some(entry => entry.address === from.address))?.[0];
  if (!name) return null;
  const family = `IPv${from.family}`;
  return {
    name,
    read: () => {
      // A file of /sys that cannot be read, as on a system without it, gives no value.
      const count = file => {
        try {
          const value = Number(readFile(`/sys/class/net/${name}/${file}`));
          return Number.isFinite(value) ? value : undefined;
        } catch {
          return undefined;
        }
      };
      const carrier = count('carrier');
      const carrierChanges = count('carrier_changes');
      return {
        ...(carrier === undefined ? {} : { carrier: carrier === 1 }),
        ...(carrierChanges === undefined ? {} : { carrierChanges }),
        address: (interfaces()[name] ?? []).some(entry => entry.family === family && !linkLocal(entry.address)),
      };
    },
  };
}

// The detailed stats walk the Mini's heaps and scan its tasks with
// interrupts masked, so they are asked for only where no rule judges video.
const TASK_STACKS_TIMEOUT_MS = 10_000;

/**
 * The row with the Mini's tasks as its detailed stats (/webrtc/stats?details=1)
 * list them at this moment (askedAt), for the place `at` in the list:
 *
 *   { taskStacks: { at, askedAt, answerMs, details, tasks } }
 *
 * details is the answer's taskStackDetails and tasks its taskStacks: every
 * task with the size of its stack, the least that was free of it since the
 * Mini's start, its core and the memory that the stack is in. Without an
 * answer the row has `error` in place of them. No rule reads the row.
 */
export async function taskStacksRow(url, at, fetchJson = address => fetch(address, { signal: AbortSignal.timeout(TASK_STACKS_TIMEOUT_MS) }).then(r => r.json())) {
  const start = Date.now();
  try {
    const answer = await fetchJson(new URL('/webrtc/stats?details=1', url));
    if (!Array.isArray(answer?.taskStacks)) throw new Error('the answer has no taskStacks');
    return { taskStacks: { at, askedAt: start, answerMs: Date.now() - start, details: answer.taskStackDetails ?? null, tasks: answer.taskStacks } };
  } catch (e) {
    return { taskStacks: { at, askedAt: start, error: e.message } };
  }
}

/**
 * clock and get are the recorder's only time and its only way to the Mini:
 * clock.now() is the time in ms, clock.sleep(ms) ends after ms, and
 * get(url, ms) is the JSON of the Mini's answer. A test passes its own.
 */
export function startRecorder(file, { page, url, switches, state, link = null, withoutFullStats = () => false, beforeRestart = () => false,
  clock = { now: Date.now, sleep }, get = getJson }) {
  const within = (promise, ms) => Promise.race([promise, clock.sleep(ms).then(() => null)]).catch(() => null);
  // A switch's sample with the time at which it was taken, for the rates.
  const stamp = sample => (sample ? { ...sample, at: clock.now() } : sample);
  // The samples of each switch that came after their time and are in no
  // row yet (withLate), and the number of samples whose state was another
  // than that of the sample before.
  const late = new Map();
  let stateChanges = 0;
  let stateBefore = null;
  // A switch's sample, or null when it does not come in time. One that comes
  // later is kept for the switch's next sample, unless the state changed in
  // between, also by the switch being off: its events are of a state that
  // has ended, whose rules do not judge the next sample.
  const ask = (name, sw) => {
    let timedOut = false;
    const asked = stateChanges;
    const answer = Promise.resolve().then(() => sw.sample()).then(stamp);
    answer.then(value => {
      if (timedOut && value && stateChanges === asked) late.set(name, withLate(late.get(name), value));
    }, () => undefined);
    return Promise.race([answer, clock.sleep(SOURCE_TIMEOUT_MS).then(() => { timedOut = true; return null; })]).catch(() => null);
  };
  const out = fs.createWriteStream(file);
  // The rows of the file so far, for the run's look at its own samples.
  const rows = [];
  const write = row => {
    rows.push(row);
    out.write(JSON.stringify(row) + '\n');
  };
  const record = row => write({ t: clock.now(), ...row });
  // The compact view keeps the poll from loading the link that it measures.
  const statsUrl = new URL('/webrtc/stats?bitrate=1', url);
  // The full view, for the counters that the compact one does not have.
  const fullStatsUrl = new URL('/webrtc/stats', url);
  let fullAskedAt = 0;
  let fullPauseMs = FULL_STATS_MS;
  // Asked after the short answer `device` of the sample that began at
  // `tick`: undefined when it is not asked for in this sample, null without
  // an answer in the sample's time.
  // While withoutFullStats(state) holds it is asked for only in the
  // recording's last sample, which no short answer follows, and gets
  // FULL_STATS_LAST_MS there.
  const fullStats = async (device, tick, current, last) => {
    const left = (last ? FULL_STATS_LAST_MS : SOURCE_TIMEOUT_MS) - (clock.now() - tick);
    // The last sample asks also without a short answer: nothing follows it.
    if (!device && !last) return undefined;
    // One request at a time: not beside the one of a state that restarts the Mini.
    if (!last && early?.pending) return undefined;
    if (!last && (withoutFullStats(current) || !fullStatsDue(tick - fullAskedAt, pauseNow(fullPauseMs, beforeRestart(current))) || left < FULL_STATS_NEEDS_MS)) return undefined;
    fullAskedAt = tick;
    const start = clock.now();
    const answer = await within(get(fullStatsUrl, left), left);
    const answerMs = answer ? clock.now() - start : null;
    fullPauseMs = fullStatsPause(fullPauseMs, answerMs);
    return answer && { answer, answerMs };
  };
  let stopped = false;
  let reads = 0;   // calls of the page for its report
  let seen = 0;    // the newest of them whose answer came in time for its sample
  // A restart of the Mini sets its counts to zero. In the first sample of a
  // state that restarts it (beforeRestart), the recorder asks for the full
  // stats once more, whatever withoutFullStats says, and does not wait for
  // them: the answer, which has FULL_STATS_LAST_MS, is in the first later
  // sample without counters of its own, with the Mini's clock of the sample
  // that asked. A sample without a short answer does not ask, and the next
  // sample of the state does.
  let restarts = false;
  let early = null;

  // One sample. last: the recording's last one, with the full stats.
  const sample = async last => {
    const tick = clock.now();
    const read = ++reads;
    const current = { ...state };
    const key = JSON.stringify(current);
    if (key !== stateBefore) {
      stateChanges++;
      late.clear();
      stateBefore = key;
    }
    const sampled = Object.entries(switches).filter(([name, sw]) => current[name] && sw.sample);
    const short = within(get(statsUrl, SOURCE_TIMEOUT_MS), SOURCE_TIMEOUT_MS);
    if (beforeRestart(current) && !restarts) {
      // While it is pending, no other request for the full stats is made,
      // so its counts are never older than counts that a row already has.
      const asked = { answer: null, pending: true };
      early = asked;
      fullAskedAt = tick;
      short.then(async device => {
        const start = clock.now();
        if (device) asked.answer = await within(get(fullStatsUrl, FULL_STATS_LAST_MS), FULL_STATS_LAST_MS);
        else restarts = false;
        Object.assign(asked, { nowMs: device?.nowMs, answerMs: clock.now() - start, pending: false });
      });
      restarts = true;
    }
    if (!beforeRestart(current)) restarts = false;
    const [device, full, browser, ...samples] = await Promise.all([
      short,
      short.then(device => fullStats(device, tick, current, last)),
      within(page.evaluate(browserReport(read, seen)), SOURCE_TIMEOUT_MS),
      ...sampled.map(([name, sw]) => ask(name, sw)),
    ]);
    if (browser) seen = read;
    const loads = Object.fromEntries(sampled.map(([name], index) => {
      const sample = withLate(late.get(name), samples[index]);
      if (samples[index]) late.delete(name);
      return [name, sample];
    }));
    const load = Math.round(os.loadavg()[0] * 100) / 100;
    // The answer from before the restart is older than this sample's own,
    // so it goes first, and the sample's own counts come again with the
    // next request: the Mini's counts only grow.
    let counters = null;
    if (early && !early.pending && early.answer) {
      counters = memoryCounters(early.answer, { nowMs: early.nowMs, answerMs: early.answerMs });
      early = null;
    }
    counters ??= memoryCounters(full?.answer, { nowMs: device?.nowMs, answerMs: full?.answerMs });
    write({ t: tick, state: current, device, counters, browser, loads, load, ...(link ? { link: link.read() } : {}) });
    return tick;
  };
  const loop = (async () => {
    while (!stopped) {
      const tick = await sample(false);
      await clock.sleep(Math.max(0, SAMPLE_MS - (clock.now() - tick)));
    }
    await sample(true);
  })();

  return {
    record,
    rows,
    // last(): rows for the end of the recording, made after the last sample.
    stop: async (last = async () => []) => {
      stopped = true;
      await loop;
      for (const row of await last()) record(row);
      await new Promise(resolve => out.end(resolve));
    },
  };
}
