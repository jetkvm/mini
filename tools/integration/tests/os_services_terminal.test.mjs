import assert from 'node:assert/strict';
import { after, before, describe, test } from 'node:test';
import { openChannel } from '../lib/channel.mjs';
import { connect, until } from '../lib/mini.mjs';
import { openSession, videoState } from '../lib/os_services.mjs';
import { senderHost } from '../lib/sender_host.mjs';

// The Mini's last message on a terminal's channel: a text message with the reason.
const CLOSING_LINE = /^\r\n\[(.*)\]\r\n$/;
// 1,288,895 bytes: the numbers 1 to 200000, one per line.
const LAST_NUMBERS = '199999\r\n200000\r\n';
const offered = state => state.osServices.connected && state.guestTerminal === true && state.guestFile?.receive === true;

// "guest-terminal": input as binary, {"rows","cols"} text sets the size, and
// the Mini closes with one text message giving the reason. Linux or macOS host.
describe('the OS Services terminal', () => {
  let mini;
  let host;
  let session;
  let skip;
  // A host that the tests are not for skips each of them.
  const step = (name, options, run) => test(name, options, t => (skip ? t.skip(skip) : run(t)));

  before(async () => {
    mini = await connect();
    try {
      host = await senderHost(mini);
      if (host.system === 'windows') {
        skip = `${host.login} runs Windows, and the tests type the commands of a POSIX shell`;
        return;
      }
      session = await openSession(mini, host);
      const settings = await mini.rpc('getOsServicesSettings');
      if (!settings.terminal_enabled || !settings.files_enabled) {
        await mini.rpc('setOsServicesSettings', { settings: { terminal_enabled: true, files_enabled: true } });
      }
      await until(() => videoState(mini), offered, 45);
    } catch (error) {
      await session?.restore().catch(() => undefined);
      await mini.close().catch(() => undefined);
      throw error;
    }
  }, { timeout: 300_000 });

  after(async () => {
    try {
      await session?.restore();
    } finally {
      await mini.close();
    }
  }, { timeout: 300_000 });

  // A terminal of rows x cols whose shell has printed its prompt.
  const openTerminal = async (rows = 24, cols = 80) => {
    const terminal = await openChannel(mini, 'guest-terminal');
    await terminal.until(state => state.readyState === 'open', 20);
    await terminal.text(JSON.stringify({ rows, cols }));
    await terminal.until(state => state.binaryBytes > 0, 20);
    return terminal;
  };
  // Types the command and returns the output after it, once it has `expected`.
  const run = async (terminal, command, expected) => {
    const from = (await terminal.state()).output.length;
    await terminal.bytes(`${command}\r`);
    return (await terminal.until(state => expected.test(state.output.slice(from)), 20)).output.slice(from);
  };
  // The channel has closed, with `reason` in its last text message and no output after it.
  const closedWith = async (terminal, reason) => {
    const state = await terminal.until(now => now.readyState === 'closed', 20);
    assert.equal(CLOSING_LINE.exec(state.texts.at(-1) ?? '')?.[1], reason, 'the reason in the closing line');
    assert.equal(state.binaryAfterText, 0, 'no output after the closing line');
    return state;
  };

  step('a terminal runs a shell, resizes, keeps up with fast output and ends with the shell\'s exit', { timeout: 240_000 }, async t => {
    let terminal = await openTerminal(24, 80);
    // The shell computes the marker, so the echo of the command does not have it.
    await run(terminal, 'echo terminal_$((6*7))', /terminal_42/);
    await terminal.text(JSON.stringify({ rows: 40, cols: 132 }));
    await run(terminal, 'stty size', /\b40 132\b/);
    // JSON that is typed is text for the shell, and no new size.
    await run(terminal, 'echo \'{"rows":3,"cols":4}\' >/dev/null; stty size', /\b40 132\b/);
    await terminal.bytes('exit\r');
    await closedWith(terminal, 'The shell exited');

    // A megabyte of output, line by line. Each message has 16 KiB at most.
    terminal = await openTerminal();
    let started = Date.now();
    let output = await run(terminal, 'seq 1 200000; echo seq_$((1+1))_done', /seq_2_done/);
    let seconds = (Date.now() - started) / 1000;
    assert.ok(output.includes(LAST_NUMBERS), 'the last number arrived');
    let state = await terminal.state();
    t.diagnostic(`line by line: ${state.binaryBytes} bytes in ${state.binaryMessages} messages, ${seconds.toFixed(1)} s`);
    assert.ok(state.binaryMessages >= Math.ceil(state.binaryBytes / (16 * 1024)), `${state.binaryBytes} bytes in ${state.binaryMessages} messages`);
    await terminal.bytes('exit\r');
    await closedWith(terminal, 'The shell exited');

    // The same in large blocks, faster than the browser takes them: the Mini
    // holds the sender's output back and does not end the terminal.
    // `command` passes by an alias of cat that colours the output.
    terminal = await openTerminal();
    started = Date.now();
    output = await run(terminal, 'seq 1 200000 | command cat; echo seq_$((2+2))_done', /seq_4_done/);
    seconds = (Date.now() - started) / 1000;
    assert.ok(output.includes(LAST_NUMBERS), 'the last number arrived');
    state = await terminal.state();
    t.diagnostic(`in blocks: ${state.binaryBytes} bytes in ${state.binaryMessages} messages, ${seconds.toFixed(1)} s`);
    await terminal.bytes('exit\r');
    await closedWith(terminal, 'The shell exited');
  });

  step('turning the terminal off in the settings ends an open one and withdraws the offer', { timeout: 120_000 }, async () => {
    const terminal = await openTerminal();
    const since = Date.now();
    try {
      await mini.rpc('setOsServicesSettings', { settings: { terminal_enabled: false } });
      await closedWith(terminal, 'Terminal disabled');
      const state = await until(() => videoState(mini), now => now.guestTerminal === undefined, 30);
      assert.deepEqual(state.guestFile, { receive: true }, 'files are still offered');
      await closedWith(await openChannel(mini, 'guest-terminal'), 'no OS Services terminal is connected');
    } finally {
      await mini.rpc('setOsServicesSettings', { settings: { terminal_enabled: true } });
    }
    await until(() => videoState(mini), offered, 30);
    await until(() => mini.timedEvents('videoInputState'),
      events => events.some(event => event.at >= since && event.params.osServices !== undefined && offered(event.params)), 10);
  });
});
