// The parts of OS Services that a session carries beside video: the terminal,
// the file transfer and the clipboard. Each is a switch (switches.mjs). The
// terminal and the file transfer run in the page, on data channels of the
// page's peer connection, as the UI opens them.
//
// A session can end while a part is on, for example when a mount takes the
// Mini off USB. A user then opens the terminal again or sends the file again,
// and so do these: the terminal and the transfer start again one second
// after their channel closes.
import { seededBytes, sha256 } from './content.mjs';
import { checksumIn } from './hosts.mjs';
import { LINE_ANSWER_MS, LINE_PERIOD_MS } from './judge.mjs';

// osFiles sends one file after the other. File n has the bytes of the seed
// n (content.mjs), so each file has a checksum of its own, which the host
// must find: a file with the bytes of another file, or with a part at a
// wrong place, has another one.
const FILE_BYTES = 1024 * 1024;
// osFilesSmall sends files of this size in the same way. Such a file is one
// message, so a drop starts several times per second, and each drop is a new
// transfer on a data channel of its own, for which the Mini reserves memory.
// A user drops one file. The tool drops many, so that one of them starts in
// the quarter of a second for which the Mini rebuilds its video capture, and
// the Mini must reserve that memory then as at any other time.
const SMALL_FILE_BYTES = 4096;

// In the page: a terminal of the tool. typed: what to type once the
// shell's prompt is there. kind: which of the tool's two terminals this is,
// of which one is open at a time. periodMs: for the light terminal, how
// often the page types a line. The page gets the three as one list.
//
// In osTerminal the host counts up, one number per line, as fast as the
// session takes it: lines counts the numbers that came, and breaks has each
// place where the next number was not the one before plus 1, as [at,
// before, number]: the page's clock at the arrival of that number, the
// number before it, and the number. The tool takes the breaks from there.
//
// In osTerminalLight the host prints "ready", and then answers each line
// "e<n>" that it gets with the line "r<n>" at once. From the host's "ready"
// on, the page types such a line every periodMs, each with the next n, and
// takes the time from its send to the arrival of the answer, on its own
// clock: sent counts the lines that it typed, answered those whose answer
// came, pending has the send of each line without an answer yet, and timed
// each round trip that has ended, as [sentAt, ms]. A new channel, after the
// session ended, waits for the host's "ready" again; the lines that were
// pending stay so (takeRoundTrips).
export const startTerminal = (typed, kind = 'osTerminal', periodMs = 100) => {
  if (Array.isArray(typed)) [typed, kind, periodMs] = typed;
  const state = { kind, bytes: 0, lines: 0, breaks: [], last: 0, rest: '', query: '', wanted: true, channel: null,
    ready: false, sent: 0, answered: 0, pending: new Map(), timed: [], typer: null };
  window.__combinedLoadTerminal = state;
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  const open = () => {
    if (!state.wanted) return;
    const peer = window.__kvmTestHooks?._getPeerConnection?.();
    if (!peer || peer.connectionState !== 'connected') return void setTimeout(open, 1000);
    const channel = peer.createDataChannel('guest-terminal');
    channel.binaryType = 'arraybuffer';
    state.channel = channel;
    state.last = 0;
    state.rest = '';
    state.query = '';
    state.ready = false;
    channel.onopen = () => {
      if (!state.wanted) return void channel.close();
      channel.send(JSON.stringify({ rows: 24, cols: 80 }));
      // After the shell's prompt.
      setTimeout(() => {
        if (state.wanted && channel.readyState === 'open') channel.send(encoder.encode(typed));
      }, 1000);
    };
    channel.onmessage = event => {
      if (typeof event.data === 'string') return;
      state.bytes += event.data.byteLength;
      const text = decoder.decode(event.data, { stream: true });
      // Windows asks where the cursor is (ESC [ 6 n) and starts the shell
      // when the terminal has answered, as a terminal program does. The
      // question can come in two messages, so the last three characters of
      // the message before are looked at again; a whole question is four.
      const seen = state.query + text;
      for (let at = seen.indexOf('\x1b[6n'); at !== -1; at = seen.indexOf('\x1b[6n', at + 4)) {
        channel.send(encoder.encode('\x1b[1;1R'));
      }
      state.query = seen.slice(-3);
      const lines = (state.rest + text).split('\n');
      state.rest = lines.pop();
      for (const line of lines) {
        // Without the control sequences that a terminal puts beside a line
        // (Windows: ESC [ ? 25 h, which shows the cursor; zsh: the window's
        // title, ESC ] ... BEL, before a command's output). The terminal of
        // macOS ends some lines with two carriage returns.
        const plain = line.replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g, '').replace(/\x1b\[[0-9;?]*[A-Za-z]/g, '').replace(/\x1b[=>]/g, '');
        if (kind === 'osTerminalLight') {
          // What the terminal shows of the line: the text after its last
          // carriage return. A shell that ends its bracketed paste with
          // ESC [ ? 2004 l puts one before the first output of the command.
          const shown = plain.replace(/\r+$/, '').split('\r').at(-1);
          if (shown === 'ready') state.ready = true;
          const answer = /^r(\d+)$/.exec(shown);
          const n = answer ? Number(answer[1]) : null;
          if (state.pending.has(n)) {
            state.timed.push([state.pending.get(n), Date.now() - state.pending.get(n)]);
            state.pending.delete(n);
            state.answered++;
          }
          continue;
        }
        const match = /^(\d+)\r*$/.exec(plain);
        if (!match) continue;
        const number = Number(match[1]);
        if (state.last && number !== state.last + 1) state.breaks.push([Date.now(), state.last, number]);
        state.last = number;
        state.lines++;
      }
    };
    channel.onclose = () => setTimeout(open, 1000);
  };
  if (kind === 'osTerminalLight') {
    state.typer = setInterval(() => {
      if (!state.wanted || !state.ready || state.channel?.readyState !== 'open') return;
      const n = ++state.sent;
      state.pending.set(n, Date.now());
      state.channel.send(encoder.encode(`e${n}\r`));
    }, periodMs);
  }
  open();
};
// In the page: the light terminal's counts, and the round trips that ended
// since the call before, as [sentAt, ms]. A line whose answer has not come
// answerMs after its send ends there, with ms null; an answer that comes
// later is not taken. null when the page has no light terminal.
export const takeRoundTrips = answerMs => {
  const state = window.__combinedLoadTerminal;
  if (!state || state.kind !== 'osTerminalLight') return null;
  const now = Date.now();
  for (const [n, sentAt] of state.pending) {
    if (now - sentAt < answerMs) continue;
    state.pending.delete(n);
    state.timed.push([sentAt, null]);
  }
  return { bytes: state.bytes, sent: state.sent, answered: state.answered, roundTrips: state.timed.splice(0) };
};
// Stops the page's terminal when it is of this kind.
const stopTerminal = kind => {
  const state = window.__combinedLoadTerminal;
  if (!state || state.kind !== kind) return;
  state.wanted = false;
  clearInterval(state.typer);
  // A channel that is still opening closes itself when it opens.
  if (state.channel?.readyState === 'open') {
    state.channel.send(new Uint8Array([3]));   // Ctrl-C
    state.channel.close();
  }
};

// In the page: one file after the other to the host, as the UI sends a
// dropped file: the Mini names a channel and a transfer, gives credit in
// bytes, and reports the file's path when the host has it. done has the
// finished transfers, each with the number of its file; the tool takes them
// from there to check them. broken counts the transfers whose channel closed
// before the Mini reported their outcome; closed counts those of them whose
// channel closed while the page's connection to the Mini was up, so that it
// was the Mini that closed it and not a connection that went away, as when
// the Mini restarts. refused counts the drops that the Mini did not take,
// and count the drops that the page began.
// content: the text of the function that makes a file's bytes from its
// number. kind: which of the tool's two file transfers this is, of which
// one runs at a time.
export const startFiles = ([size, content, kind = 'osFiles']) => {
  const state = { kind, bytesSent: 0, broken: 0, closed: 0, refused: 0, done: [], wanted: true, channel: null, count: 0 };
  window.__combinedLoadFiles = state;
  const bytesOf = (0, eval)(`(${content})`);
  const again = () => setTimeout(next, 1000);
  const next = () => {
    if (!state.wanted) return;
    const hooks = window.__kvmTestHooks;
    const peer = hooks?._getPeerConnection?.();
    if (!peer || peer.connectionState !== 'connected') return void again();
    const file = ++state.count;
    const bytes = bytesOf(file, size);
    hooks.sendJsonRpc('guestFileDrop', { drop: { files: [{ name: `combined-load-${file}.bin`, size }] } }, answer => {
      if (answer.error && state.wanted) state.refused++;
      if (answer.error || !state.wanted) return void again();
      const transfer = answer.result.transfers[0];
      const channel = peer.createDataChannel(answer.result.channel);
      channel.binaryType = 'arraybuffer';
      state.channel = channel;
      // The UI's numbers: 32 KiB a message, sending paused above 96 KiB.
      const chunk = 32 * 1024;
      const highWater = 96 * 1024;
      let sent = 0;
      let credit = 0;
      let finished = false;
      let pumping = false;
      const pump = async () => {
        if (pumping) return;
        pumping = true;
        while (state.wanted && !finished && credit > 0 && sent < size && channel.readyState === 'open') {
          if (channel.bufferedAmount > highWater) {
            await new Promise(resolve => setTimeout(resolve, 20));
            continue;
          }
          const n = Math.min(chunk, credit, size - sent);
          const message = new Uint8Array(4 + n);
          new DataView(message.buffer).setUint32(0, transfer, true);
          message.set(bytes.subarray(sent, sent + n), 4);
          channel.send(message);
          sent += n;
          credit -= n;
          state.bytesSent += n;
        }
        pumping = false;
      };
      // A channel that opens after the stop is closed, and sends nothing.
      channel.onopen = () => (state.wanted ? void pump() : channel.close());
      channel.onmessage = event => {
        if (typeof event.data !== 'string') return;
        const message = JSON.parse(event.data);
        if (message.type === 'credit') {
          credit += message.bytes ?? 0;
          void pump();
        } else if (message.type === 'done') {
          finished = true;
          // A transfer that the tool cut off when it stopped does not count.
          if (state.wanted) state.done.push({ file, status: message.status, path: message.path });
          channel.close();
        }
      };
      // The next file at once after a finished one, else after a second.
      channel.onclose = () => {
        if (!finished && state.wanted) {
          state.broken++;
          if (peer.connectionState === 'connected') state.closed++;
        }
        if (state.wanted) setTimeout(next, finished ? 0 : 1000);
        finished = true;
      };
    }, 10_000);
  };
  next();
};
// Stops the page's file transfer when it is of this kind.
const stopFiles = kind => {
  const state = window.__combinedLoadFiles;
  if (!state || state.kind !== kind) return;
  state.wanted = false;
  if (state.channel?.readyState === 'open') state.channel.close();
};

/**
 * page: the tool's page. call: one RPC through the page, without a wait for
 * the RPC channel. hostExec: a command on the host. host: the host's
 * commands (hosts.mjs).
 */
export function osServicesParts({ page, call, hostExec, host }) {
  // Files of `bytes` bytes to the host, one after the other. The page has
  // one file transfer of the tool at a time, of the kind `name`. The host's
  // copy of each file is checked against the file's checksum and removed.
  // failed has every transfer that did not end with the right file; corrupt
  // has those of them that the Mini reported as done and whose file on the
  // host has another checksum.
  const fileTransfer = (name, bytes) => {
    const files = { verified: 0, failed: 0, corrupt: 0, lastFailure: null, checking: null, timer: null };
    // One check at a time; a call during a check waits for it and does no other.
    const check = () => (files.checking ??= checkOnce().finally(() => { files.checking = null; }));
    const checkOnce = async () => {
      try {
        const done = await page.evaluate(() => window.__combinedLoadFiles?.done.splice(0) ?? []);
        // The host's checksums of all of them from one command and one
        // process: a round has tens of small files, and a process for each
        // falls behind them on a Windows host.
        const onHost = done.filter(({ status, path }) => status === 'ok' && path).map(({ path }) => path);
        const printed = onHost.length ? await hostExec(host.checksumsAndRemove(onHost)).catch(() => '') : '';
        for (const { file, status, path } of done) {
          const sum = status === 'ok' && path ? checksumIn(printed, path) : '';
          if (sum === sha256(seededBytes(file, bytes))) {
            files.verified++;
          } else {
            files.failed++;
            // A checksum of the host that is another one; without a checksum
            // the host did not have the file, or did not answer.
            if (/^[0-9a-f]{64}$/.test(sum)) files.corrupt++;
            files.lastFailure = `file ${file}, status ${status}, path ${path}, checksum ${sum || 'none'}`;
          }
        }
      } catch {
        // The page is between two documents; the next round takes them.
      }
    };
    return {
      on: async () => {
        Object.assign(files, { verified: 0, failed: 0, corrupt: 0, lastFailure: null });
        await page.evaluate(startFiles, [bytes, String(seededBytes), name]);
        files.timer = setInterval(check, 1000);
      },
      off: async () => {
        clearInterval(files.timer);
        files.timer = null;
        await page.evaluate(stopFiles, name).catch(() => undefined);
        // Removes the last files from the host, after a check that is under
        // way. A file that was finished in the last second before off is
        // not judged: no sample follows.
        await check();
        await check();
      },
      isOn: () => files.timer !== null,
      // The host has taken the first 256 KiB, or the first file when a file
      // is smaller.
      works: async on => !on || (await page.evaluate(() => window.__combinedLoadFiles?.bytesSent ?? 0)) >= Math.min(256 * 1024, bytes),
      // started has the drops that the page began.
      sample: async () => {
        const sent = await page.evaluate(() => {
          const { bytesSent, broken, closed, refused, count } = window.__combinedLoadFiles ?? {};
          return bytesSent === undefined ? null : { bytesSent, broken, closed, refused, started: count };
        });
        return sent && { ...sent, verified: files.verified, failed: files.failed, corrupt: files.corrupt, lastFailure: files.lastFailure };
      },
    };
  };

  // osClipboard: once per second a new text to the host's clipboard and back.
  // wrong has every round that did not bring the text back; corrupt has those
  // of them in which the host answered with another text. ended has each
  // round that ended since the sample before, as [startedAt, ms, back]: its
  // start, its time, and true when the text came back as it was set, false
  // when the host answered with another text, null when a call failed or had
  // no answer.
  const clipboard = { rounds: 0, wrong: 0, corrupt: 0, ended: [], busy: false, startedAt: 0, timer: null };
  const clipboardRound = async () => {
    if (clipboard.busy) return;
    clipboard.busy = true;
    const text = `combined-load ${Date.now()}`;
    const start = Date.now();
    clipboard.startedAt = start;
    try {
      await call('guestClipboardSet', { text }, 3000);
      const back = await call('guestClipboardGet', {}, 3000);
      if (back.text !== text) {
        clipboard.wrong++;
        clipboard.corrupt++;
      }
      clipboard.ended.push([start, Date.now() - start, back.text === text]);
    } catch {
      clipboard.wrong++;
      clipboard.ended.push([start, Date.now() - start, null]);
    }
    clipboard.rounds++;
    clipboard.busy = false;
  };

  // The terminal of the kind `name`, into which the tool types `typed`. The
  // page has one terminal of the tool at a time.
  const terminal = (name, typed) => ({
      on: () => page.evaluate(startTerminal, [typed, name, LINE_PERIOD_MS]),
      off: () => page.evaluate(stopTerminal, name).catch(() => undefined),
      isOn: () => page.evaluate(kind => window.__combinedLoadTerminal?.kind === kind && window.__combinedLoadTerminal.wanted === true, name)
        .catch(() => false),
      // The first numbers have come, or the first answer of the host.
      works: async on => !on || (await page.evaluate(([kind]) => {
        const terminal = window.__combinedLoadTerminal;
        return kind === 'osTerminalLight' ? (terminal?.answered ?? 0) > 0 : (terminal?.lines ?? 0) > 100;
      }, [name])),
      // osTerminal: the bytes and the numbers so far, and the breaks in the
      // numbers since the sample before. osTerminalLight: the bytes and the
      // lines so far, those answered, and the round trips that ended since
      // the sample before (takeRoundTrips).
      sample: () => (name === 'osTerminalLight' ? page.evaluate(takeRoundTrips, LINE_ANSWER_MS)
        : page.evaluate(() => {
          const terminal = window.__combinedLoadTerminal;
          if (!terminal) return null;
          const { bytes, lines } = terminal;
          return { bytes, lines, breaks: terminal.breaks.splice(0) };
        })),
  });

  return {
    // A terminal in which the host prints numbers, one per line, without
    // end, as fast as the session takes them.
    osTerminal: terminal('osTerminal', host.countInTerminal),
    // A terminal into which the page types ten short lines per second, as a
    // user types commands, and in which the host answers each line at
    // once: the answer arrives without a wait behind other output, so the
    // round trip of a line is the session's.
    osTerminalLight: terminal('osTerminalLight', host.answerInTerminal),

    // A 1 MiB file to the host, again and again, for the rate of a transfer.
    osFiles: fileTransfer('osFiles', FILE_BYTES),
    // A 4 KiB file to the host, again and again, for the start of a transfer.
    osFilesSmall: fileTransfer('osFilesSmall', SMALL_FILE_BYTES),

    osClipboard: {
      on: async () => {
        Object.assign(clipboard, { rounds: 0, wrong: 0, corrupt: 0, ended: [] });
        clipboard.timer = setInterval(clipboardRound, 1000);
      },
      off: async () => {
        clearInterval(clipboard.timer);
        clipboard.timer = null;
      },
      isOn: () => clipboard.timer !== null,
      // A text has come back as it was set.
      works: on => !on || clipboard.rounds > clipboard.wrong,
      // The counts so far, the rounds that ended since the sample before,
      // and runningSince, the start of the round that is under way, null
      // without one, so that the judge counts a slow round in every second
      // of it with the time it has taken by the sample (`at`).
      sample: () => {
        const { rounds, wrong, corrupt } = clipboard;
        return { rounds, wrong, corrupt, ended: clipboard.ended.splice(0), runningSince: clipboard.busy ? clipboard.startedAt : null };
      },
    },
  };
}
