#!/usr/bin/env node
// What became of the keys and the moves of the mouse that `hid` sent: the
// time of each key to the Mini's answer, the keys and the moves that the
// host did not get, and the longest time between two moves on the host.
// The samples of `hid` have the single events of their second, and the
// judge counts the lost keys and moves and takes the longest time between
// two moves from them (hostInputAt), so a recording can be judged again
// with other times. A sample has:
//
//   sends    [usage, sentAt]: a key that the tool sent in that second, at
//            the time at which it asked the page for the click, on this
//            machine's clock
//   keys     [usage, sentAt, latencyMs]: a key that the tool judged, the
//            time at which the page sent it, and the time from that send
//            to the Mini's message that lists the key as down, both on the
//            page's clock; null when no such message came within
//            KEY_ANSWER_MS
//   moves    [x, sentAt, movement]: the place of a move of the mouse, the
//            time at which the page sent it, and the number of the hand's
//            movement that it is part of
//   reports  [what, hostMs, readAt]: a line of the host's key watch,
//            "p<usage>" for a key, "m<x>" for a move to x and "." for its
//            beat; for a move the time at which the host got it, on the
//            host's clock, else null; and this machine's clock when the
//            tool read the line
//
// The page runs in this machine's browser, so its clock is this machine's.
// The host's clock is not this machine's, and no time of the host is
// compared with a time of this machine. A key or a move of the host belongs
// to its send by what it is: a move has its own place, and a key comes
// again only after the other keys of HID_KEYS, later than KEY_LOST_MS. The
// times of the host are only compared with each other.
//
//   node tools/combined_load/runner/hid_times.mjs <recording.jsonl> [--all]
//
// prints, for each time that hid was on, the median and the slowest time of
// a key to the Mini's answer, the keys without an answer, the lost keys and
// moves, and the longest time between two moves on the host; and each key
// whose answer took more than 100 ms, each lost key, and each time of more
// than 100 ms between two moves on the host. --all prints every key.
import { fileURLToPath } from 'node:url';
import { HID_KEYS } from './hosts.mjs';
import { HOST_LINE_GRACE_MS, KEY_ANSWER_MS, KEY_LOST_MS, MOVE_LOST_MS, readRows } from './judge.mjs';

/** One line of a host's key watch and the time it was read, as a report, or null for another line. */
export function parseReport(line, readAt) {
  const [what, time] = line.trim().split(' ');
  if (what !== '.' && !/^[pm]\d+$/.test(what)) return null;
  const hostMs = what.startsWith('m') && time !== undefined && time !== '' && Number.isFinite(Number(time)) ? Number(time) : null;
  return [what, hostMs, readAt];
}

/**
 * In the page, before it loads (page.addInitScript): keeps each message of
 * the Mini that says which keys are down, with the page's clock at its
 * arrival, in window.__combinedLoadKeyStates as [at, keys]. The page sends
 * nothing for it. The message comes on the "hidrpc" data channel as bytes:
 * 0x33, the modifier, then the keys; the data is a Blob or an ArrayBuffer.
 * While the HID RPC handshake is not complete the Mini sends it on the "rpc"
 * channel as the JSON-RPC notification keysDownState with { modifier, keys }.
 * The Mini sends it after the USB has taken the key report
 * (components/jetkvm_hid_input/jetkvm_hid_input.c note_keyboard_changed,
 * components/webrtc_transport/webrtc_transport.c publish_keyboard_change).
 * The page's channels are known only to the page, so the script takes each
 * channel as the page makes it. While no switch takes the messages, the
 * newest 1000 are kept.
 *
 * window.__combinedLoadKeys.press(usage) presses a key through the page's
 * own handler of a key and returns the page's clock at the press. The key
 * stays down until a message lists it, and goes up at once in the handler
 * of that message, or after holdMs without one. A key that goes up before
 * the Mini has read the keyboard's state is in no message: the Mini reads
 * the state after the USB has taken the reports, and then has the keys
 * after the release. While the key is down the page's handler sends its
 * keepalives, which keep the Mini from releasing the key itself after
 * 100 ms (JETKVM_HID_INPUT_KEYPRESS_RELEASE_MS). releaseAll() releases the
 * keys that are down.
 */
export function watchKeyStates(holdMs = 500) {
  const states = [];
  window.__combinedLoadKeyStates = states;
  const down = new Map();   // usage -> the timer that releases it
  const release = usage => {
    if (!down.has(usage)) return;
    clearTimeout(down.get(usage));
    down.delete(usage);
    window.__kvmTestHooks?.sendKeypress(usage, false);
  };
  window.__combinedLoadKeys = {
    press: usage => {
      const at = Date.now();
      release(usage);
      window.__kvmTestHooks.sendKeypress(usage, true);
      down.set(usage, setTimeout(() => release(usage), holdMs));
      return at;
    },
    releaseAll: () => { for (const usage of [...down.keys()]) release(usage); },
  };
  const keep = (at, keys) => {
    states.push([at, keys]);
    if (states.length > 1000) states.shift();
    for (const usage of keys) release(usage);
  };
  const onMessage = async event => {
    const at = Date.now();
    if (typeof event.data === 'string') {
      if (!event.data.includes('keysDownState')) return;
      try {
        const message = JSON.parse(event.data);
        if (message.method === 'keysDownState') keep(at, Array.from(message.params?.keys ?? []));
      } catch {
        // Not JSON-RPC.
      }
      return;
    }
    const bytes = new Uint8Array(event.data instanceof Blob ? await event.data.arrayBuffer() : event.data);
    if (bytes[0] === 0x33) keep(at, Array.from(bytes.slice(2)));
  };
  const create = RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel = function (label, ...rest) {
    const channel = create.call(this, label, ...rest);
    if (label === 'hidrpc' || label === 'rpc') channel.addEventListener('message', onMessage);
    return channel;
  };
}

/**
 * In the page: the hand of `hid`, which moves the mouse for movesMs and
 * rests for restsMs, again and again, until stop(). While it moves it sends
 * one move per periodMs, to places.count places in turn, places.step apart
 * from places.first on, at the height y. Its timer fires four times per
 * period, and the first tick of a period sends that period's move, so a
 * move is at most a quarter of a period late. A tick that comes after its
 * period sends one move and not the ones it missed, as a browser gives a
 * page one move per frame of its screen. take() returns the place, the
 * time and the number of the movement of each move sent since the call
 * before.
 */
export function moveHand([periodMs, movesMs, restsMs, places, y]) {
  window.__combinedLoadMouse?.stop();
  const sent = [];
  const start = performance.now();
  let period = '';
  let count = 0;
  const timer = setInterval(() => {
    const elapsed = performance.now() - start;
    // The periods are counted from the start of each movement.
    const movement = Math.floor(elapsed / (movesMs + restsMs));
    const inMovement = elapsed % (movesMs + restsMs);
    const now = `${movement}:${Math.floor(inMovement / periodMs)}`;
    if (inMovement >= movesMs || now === period) return;
    period = now;
    const x = places.first + (count++ % places.count) * places.step;
    sent.push([x, Date.now(), movement]);
    window.__kvmTestHooks?.sendAbsMouseMove(x, y, 0);
  }, periodMs / 4);
  window.__combinedLoadMouse = { stop: () => clearInterval(timer), take: () => sent.splice(0) };
}

/**
 * The time of each key from the page's send to the Mini's message that
 * lists it as down, both on the page's clock.
 *
 *   send(usage, at)  the page sent the key at `at`
 *   states(list)     the Mini's messages of the keys that are down, as
 *                    [at, keys], in any order
 *   judge(now)       returns the keys judged by now, each once: answered,
 *                    or sent answerMs before now without an answer, as
 *                    [usage, sentAt, latencyMs], null without an answer
 *
 * A message answers each key that it lists and that was sent before it. A
 * key comes again only after the other keys, so a message is not taken for
 * the send of the key before. A message that comes after the key was judged
 * without an answer answers nothing.
 */
export function keyAnswers({ answerMs = KEY_ANSWER_MS } = {}) {
  let sends = [];   // not yet judged, oldest first
  return {
    send: (usage, at) => { sends.push({ usage, at, latencyMs: null }); },
    states: list => {
      for (const [at, keys] of list) {
        for (const send of sends) {
          if (send.at <= at && keys.includes(send.usage)) send.latencyMs = Math.min(send.latencyMs ?? Infinity, at - send.at);
        }
      }
    },
    judge: now => {
      const judged = send => send.latencyMs !== null || send.at <= now - answerMs;
      const keys = sends.filter(judged).map(send => [send.usage, send.at, send.latencyMs]);
      sends = sends.filter(send => !judged(send));
      return keys;
    },
  };
}

/**
 * Finds the keys that the host did not get. The host's key watch says only
 * which key the host got, without a time. A key is lost when the tool has
 * not read the host's report of it within lostMs of its send, both times on
 * this machine's clock. A key comes again only after the other keys of
 * HID_KEYS, later than lostMs, so a report read in that time is of that
 * send.
 *
 *   send(usage, at)       the tool sent the key at `at`
 *   report(usage, readAt) the tool read the host's report of the key at
 *                         readAt
 *   judge(now)            returns the keys sent lostMs or more before now
 *                         that have no report, each once, as
 *                         { usage, sentAt }
 */
export function hostKeys({ lostMs = KEY_LOST_MS } = {}) {
  let sends = [];   // not yet judged and not reported, oldest first
  return {
    send: (usage, at) => { sends.push({ usage, at }); },
    report: (usage, readAt) => {
      sends = sends.filter(send => send.usage !== usage || send.at > readAt || readAt - send.at > lostMs);
    },
    judge: now => {
      const lost = sends.filter(send => send.at <= now - lostMs).map(send => ({ usage: send.usage, sentAt: send.at }));
      sends = sends.filter(send => send.at > now - lostMs);
      return lost;
    },
  };
}

/**
 * Counts the moves of the mouse that the host did not get, and takes the
 * longest time between two moves that the host got. A move has its own
 * place, so the report of a place is the report of the move that was sent
 * there, each move on its own.
 *
 *   send(x, at, movement)      the page sent a move to x at `at`, in that
 *                              movement of the hand
 *   report(x, hostMs, readAt)  a line of the watch: the host got a move to
 *                              x at hostMs on its own clock, and the tool
 *                              read the line at readAt on this one
 *   judge(now)                 judges the moves that are lostMs and the
 *                              grace old, by when the line of a move that
 *                              the host got has come: a move without a
 *                              report is lost. Returns the lost moves so
 *                              far, { mouseLost }; of the moves that this
 *                              call judged, the longest time between two
 *                              moves of one movement that the host got one
 *                              after the other, on the host's clock,
 *                              { mouseGapMs }, null without such a pair;
 *                              and the time at which the page sent the
 *                              second move of that pair, { gapAt }
 *
 * A pair can have lost moves between its two: the time between them is the
 * time in which the pointer did not move on the host. The time from the
 * last move of a movement to the first of the next has the hand's rest in
 * it, and is not taken.
 */
export function hostMoves({ lostMs = MOVE_LOST_MS, graceMs = HOST_LINE_GRACE_MS } = {}) {
  const sends = [];            // not yet judged, oldest first
  const reports = new Map();   // place -> the newest report of it that no send has taken
  let mouseLost = 0;
  let last = null;             // the newest judged move that the host got: { movement, hostMs }
  return {
    send: (x, at, movement) => { sends.push({ x, at, movement }); },
    report: (x, hostMs, readAt) => { reports.set(x, { hostMs, readAt }); },
    judge: now => {
      let gap = null;
      while (sends.length && sends[0].at <= now - lostMs - graceMs) {
        const send = sends.shift();
        const report = reports.get(send.x);
        // The report of a send is one that the tool read after the send.
        if (!report || report.readAt < send.at) {
          mouseLost++;
          continue;
        }
        reports.delete(send.x);
        if (last?.movement === send.movement && last.hostMs !== null && report.hostMs !== null &&
            (gap === null || report.hostMs - last.hostMs > gap.ms)) {
          gap = { ms: report.hostMs - last.hostMs, at: send.at };
        }
        last = { movement: send.movement, hostMs: report.hostMs };
      }
      // A report that no send has taken by now belongs to none.
      for (const [x, report] of reports) {
        if (report.readAt < now - 2 * (lostMs + graceMs)) reports.delete(x);
      }
      return { mouseLost, mouseGapMs: gap && Math.round(gap.ms * 10) / 10, gapAt: gap?.at ?? null };
    },
  };
}

/**
 * What the host got of the keys and the moves of hid, from the events of the
 * samples (see the top of this file). samples: the samples of a recording.
 * lostMs and graceMs: the times after which the judge takes a move as lost
 * (hostMoves). Returns one entry for each time that hid was on, a list of
 * its samples in a row that have the switch's sample, and for each of them
 * { sample, first, lostKeys, mouseLost, mouseGapMs, gapAt }:
 *   first       the sample is the first of that time: no sample before it
 *               says what was there before its second
 *   lostKeys    the keys whose report the tool did not read within
 *               KEY_LOST_MS of their send, { usage, sentAt }; such a key is
 *               counted in the first sample at least KEY_LOST_MS after its
 *               send. The last sample of that time counts the keys whose
 *               KEY_LOST_MS have passed by its newest event or by the
 *               sample after that time, whichever is later; a key whose
 *               time has not passed then is not judged, as a recording that
 *               ends while hid is on, or the lines that come after hid is
 *               off, cannot say whether the host got it
 *   mouseLost   the moves that the judge of this sample took as lost
 *   mouseGapMs  of the moves that it judged, the longest time between two
 *               moves of one movement on the host's clock, null without
 *               such a pair; gapAt the time at which the page sent the
 *               second move of that pair
 * The events of all the samples of a time are taken in the order of their
 * times on this machine's clock, a send before a report at the same time,
 * and at each sample those up to its time, as the tool would have had them
 * then; at its last sample, all of them. A move is taken from the page up to
 * one key period after its send, and so can be in the sample after its time.
 * A sample of that time without
 * the switch's sample has no entry: what was judged at its time is in the
 * next entry, as a count that grows is.
 */
export function hostInput(samples, { lostMs = MOVE_LOST_MS, graceMs = HOST_LINE_GRACE_MS, keyLostMs = KEY_LOST_MS } = {}) {
  // Each time that hid was on, with the time of the sample after it, null
  // when the samples end with hid on.
  const runs = [];
  let run = null;
  for (const sample of samples) {
    if (!sample.state?.hid) {
      if (run) run.offAt = sample.t;
      run = null;
      continue;
    }
    if (!run) runs.push(run = { samples: [], offAt: null });
    run.samples.push(sample);
  }
  return runs.map(({ samples: run, offAt }) => {
    const host = hostKeys({ lostMs: keyLostMs });
    const mouse = hostMoves({ lostMs, graceMs });
    const hid = run.map(sample => sample.loads?.hid).filter(Boolean);
    let lostKeys = [];
    const events = [
      ...hid.flatMap(one => one.sends ?? []).map(([usage, sentAt]) => [sentAt, 0, () => host.send(usage, sentAt)]),
      ...hid.flatMap(one => one.moves ?? []).map(([x, sentAt, movement]) => [sentAt, 0, () => mouse.send(x, sentAt, movement)]),
      ...hid.flatMap(one => one.reports ?? []).map(([what, hostMs, readAt]) => [readAt, 1, () => {
        if (what.startsWith('p')) host.report(Number(what.slice(1)), readAt);
        if (what.startsWith('m')) mouse.report(Number(what.slice(1)), hostMs, readAt);
      }]),
    ].sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    let next = 0;
    let lost = 0;
    let first = true;
    let gap = null;   // the longest time between two moves since the entry before: { ms, at }
    const entries = [];
    for (const sample of run) {
      const end = sample === run.at(-1) ? Infinity : sample.t;
      while (next < events.length && events[next][0] <= end) events[next++][2]();
      // The last sample judges the keys up to the newest time that the
      // recording knows of this time of hid.
      lostKeys.push(...host.judge(end === Infinity ? Math.max(sample.t, events.at(-1)?.[0] ?? sample.t, offAt ?? sample.t) : end));
      const { mouseLost, mouseGapMs, gapAt } = mouse.judge(sample.t);
      if (mouseGapMs !== null && (gap === null || mouseGapMs > gap.ms)) gap = { ms: mouseGapMs, at: gapAt };
      if (!sample.loads?.hid) continue;
      entries.push({ sample, first, lostKeys, mouseLost: mouseLost - lost, mouseGapMs: gap?.ms ?? null, gapAt: gap?.at ?? null });
      first = false;
      lostKeys = [];
      lost = mouseLost;
      gap = null;
    }
    return entries;
  }).filter(entries => entries.length);
}

/**
 * The judge's measures of what the host got, for each sample of hid:
 * Map(sample -> { keysLost, mouseMovesLost, mouseGapMs }), from hostInput()
 * with its times. The first sample of a time that hid was on counts no lost
 * key or move, as a count that grows has nothing to grow from there; a
 * sample without a pair of moves has no mouseGapMs.
 */
export function hostInputAt(samples, times = {}) {
  return new Map(hostInput(samples, times).flat().map(({ sample, first, lostKeys, mouseLost, mouseGapMs }) => [sample, {
    keysLost: first ? undefined : lostKeys.length,
    mouseMovesLost: first ? undefined : mouseLost,
    mouseGapMs: mouseGapMs ?? undefined,
  }]));
}

/**
 * rows: the rows of a recording. Returns one entry for each time that hid
 * was on, { from, to }, the times of its first and its last sample, with:
 *   keys       the judged keys, { usage, sentAt, latencyMs }
 *   lostKeys   the keys whose report the tool did not read within
 *              KEY_LOST_MS of their send, { usage, sentAt }
 *   moves      the count of the moves that the page sent
 *   lostMoves  for each sample in which moves were lost, { t, count }
 *   gaps       for each sample with a pair of moves, the longest time
 *              between two moves on the host, { gapMs, at }, at being the
 *              send of the second move
 * as the judge takes them (hostInput). The moves sent in the last seconds of
 * such a time are not judged.
 */
export function hidTimes(rows) {
  return hostInput(rows.filter(row => row.state && !row.change)).map(run => ({
    from: run[0].sample.t,
    to: run.at(-1).sample.t,
    keys: run.flatMap(({ sample }) => sample.loads.hid.keys ?? []).map(([usage, sentAt, latencyMs]) => ({ usage, sentAt, latencyMs })),
    lostKeys: run.flatMap(entry => entry.lostKeys),
    moves: run.reduce((sum, { sample }) => sum + (sample.loads.hid.moves?.length ?? 0), 0),
    lostMoves: run.filter(entry => entry.mouseLost > 0).map(entry => ({ t: entry.sample.t, count: entry.mouseLost })),
    gaps: run.filter(entry => entry.mouseGapMs !== null).map(entry => ({ gapMs: entry.mouseGapMs, at: entry.gapAt })),
  }));
}

const NAMES = new Map(HID_KEYS.map(key => [key.usage, key.name]));
const SLOW_MS = 100;
const ms = value => (value === null || value === undefined ? '      -' : value.toFixed(1).padStart(7));

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [file, option] = process.argv.slice(2);
  if (!file) {
    console.error('usage: hid_times.mjs <recording.jsonl> [--all]');
    process.exit(2);
  }
  const rows = readRows(file);
  const start = rows[0].t;
  const second = t => `${((t - start) / 1000).toFixed(1).padStart(6)} s`;
  const name = usage => NAMES.get(usage) ?? usage;
  for (const { from, to, keys, lostKeys, moves, lostMoves, gaps } of hidTimes(rows)) {
    const latencies = keys.map(key => key.latencyMs).filter(value => value !== null).sort((a, b) => a - b);
    const longest = gaps.reduce((a, b) => (a === null || b.gapMs > a.gapMs ? b : a), null);
    console.log(`hid from ${second(from)} to ${second(to)}`);
    console.log(`  ${keys.length} keys: to the Mini's answer median ${ms(latencies[Math.floor(latencies.length / 2)])} ms, ` +
      `slowest ${ms(latencies.at(-1))} ms; ${keys.length - latencies.length} without an answer; ${lostKeys.length} lost on the way to the host`);
    console.log(`  ${moves} moves: ${lostMoves.reduce((sum, entry) => sum + entry.count, 0)} lost; ` +
      `longest time between two moves on the host ${ms(longest?.gapMs)} ms`);
    for (const key of keys) {
      if (option === '--all' || key.latencyMs === null || key.latencyMs > SLOW_MS) {
        console.log(`at ${second(key.sentAt)}  key ${name(key.usage)}  ${key.latencyMs === null ? 'no answer' : `${ms(key.latencyMs)} ms to the answer`}`);
      }
    }
    for (const key of lostKeys) console.log(`at ${second(key.sentAt)}  key ${name(key.usage)}  lost`);
    for (const entry of lostMoves) console.log(`by ${second(entry.t)}  ${entry.count} moves lost`);
    for (const gap of gaps) if (gap.gapMs > SLOW_MS) console.log(`at ${second(gap.at)}  ${ms(gap.gapMs)} ms between two moves on the host`);
  }
}
