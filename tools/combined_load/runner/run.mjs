#!/usr/bin/env node
// Takes a Mini through the lists of states in ../lists.jsonc, records what the
// device, the browser and the host report once per second, and judges each
// recording against the criteria and known failures of the Mini's board, in
// ../ethernet or ../wifi (README.md).
//
//   node tools/combined_load/runner/run.mjs <bench> --image=<file> [--install]
//       [--list=<name>] [--without=<variable>,...] [--out=<dir>] [--plain-cloud]
//
// <bench> is a file in tools/benches/ (name without .env) or a path to
// one; uses JETKVM_URL, JETKVM_REMOTE_HOST (or JETKVM_COMBINED_LOAD_HOST),
// JETKVM_SKU and, for cloud, JETKVM_MOCK_CLOUD_JWKS_URL. --image is the
// firmware the Mini runs; ota reinstalls it from the tool's update server.
// --install installs it first if the Mini runs another build. Each list
// starts from the clean state. --plain-cloud serves the cloud over http/ws.
//
// Exit status: 0 every list passed, 1 a list failed or is undecided, 2 a
// list did not take place, 3 only known faults.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { benchFile as findBenchFile, readBench } from './build.mjs';
import { drive, until, validateList, withoutVariables } from './driver.mjs';
import { BOARDS, EXIT, changeLimit, changeLine, expand, judge, readJsonc, readRows, report, secondsNotNormal, toolVersion, validateCriteria, validateKnown } from './judge.mjs';
import { hardwareVideoDecoders, launchBrowser } from './page.mjs';
import { startRecorder, taskStacksRow, toolLink } from './recorder.mjs';
import { MOUNTS, VARIABLES, createSwitches, senderOverride } from './switches.mjs';
import { SENDER_PROTOCOL_ERROR, senderLogCommands } from '../../integration/lib/sender_host.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..', '..');
const [benchArg, ...options] = process.argv.slice(2);
const args = Object.fromEntries(options.map(a => {
  const [k, ...v] = a.replace(/^--/, '').split('=');
  return [k, v.join('=') || true];
}));
if (!benchArg || benchArg.startsWith('--')) {
  console.error('usage: run.mjs <bench> --image=<file> [--install] [--list=<name>] [--without=<variable>,...] [--out=<dir>] [--plain-cloud]');
  process.exit(2);
}

const benchFile = findBenchFile(root, benchArg);
const bench = readBench(benchFile);
const url = bench.JETKVM_URL;
const remoteHost = bench.JETKVM_COMBINED_LOAD_HOST || bench.JETKVM_REMOTE_HOST;
if (!url || !remoteHost) {
  console.error(`${benchFile} must set JETKVM_URL and JETKVM_REMOTE_HOST or JETKVM_COMBINED_LOAD_HOST`);
  process.exit(2);
}

// The board has its own limits and known failures.
const board = BOARDS[bench.JETKVM_SKU];
if (!board) {
  console.error(`${benchFile}: JETKVM_SKU must be one of ${Object.keys(BOARDS).join(', ')}`);
  process.exit(2);
}
const lists = readJsonc(path.join(here, '..', 'lists.jsonc'));
const criteria = readJsonc(path.join(here, '..', board, 'criteria.jsonc'));
const known = readJsonc(path.join(here, '..', board, 'known-failures.jsonc'));
for (const list of Object.values(lists)) validateList(list, VARIABLES);
// --without leaves out every state in which one of the named variables is on:
// ota for a repeat walk of the same build, the TF-card variables on a Mini
// without a card.
const without = args.without ? String(args.without).split(',') : [];
const unknown = without.find(name => !VARIABLES.includes(name));
if (unknown !== undefined) {
  console.error(`--without: unknown variable "${unknown}"; the variables are ${VARIABLES.join(', ')}`);
  process.exit(2);
}
// The states with tailscale join a real tailnet with the auth key in the file
// that JETKVM_TAILSCALE_AUTH_KEY_FILE names. A bench without the key cannot
// have them, so they are left out there.
const tailscaleKeyFile = process.env.JETKVM_TAILSCALE_AUTH_KEY_FILE;
if (!tailscaleKeyFile && !without.includes('tailscale')) {
  console.error('JETKVM_TAILSCALE_AUTH_KEY_FILE is not set: the states with tailscale are left out');
  without.push('tailscale');
}
for (const name of Object.keys(lists)) lists[name] = withoutVariables(lists[name], without);
const empty = Object.keys(lists).find(name => lists[name].length === 0);
if (empty !== undefined) {
  console.error(`--without=${args.without} leaves no state in the list "${empty}"`);
  process.exit(2);
}
validateCriteria(criteria, VARIABLES);
validateKnown(known, VARIABLES);
// How far before a change the run looks at its recording to find when the
// state after the change is settled: a minute more than the longest time
// for which an entry of the known failures tolerates a breach and applies
// after its state.
const SETTLE_LOOKS_BACK_MS = 60_000 + 1000 *
  Math.max(0, ...expand(known).map(entry => (entry.tolerateSeconds ?? 0) + (entry.recoverySeconds ?? 0)));
const names = args.list ? [args.list] : Object.keys(lists);
if (names.some(name => !lists[name])) {
  console.error(`lists.jsonc has no list "${args.list}"; it has ${Object.keys(lists).join(', ')}`);
  process.exit(2);
}

const out = args.out || path.join(root, '.e2e-out', `combined-load-${new Date().toISOString().replace(/[:.]/g, '-')}`);
if (!args.image && (args.install || names.some(name => lists[name].some(step => step.state.ota)))) {
  console.error('a list has ota, or --install is given; pass --image=<the firmware image that the Mini runs>');
  process.exit(2);
}
if (args.image && !fs.existsSync(args.image)) {
  console.error(`no firmware image: ${args.image}`);
  process.exit(2);
}
// The tool's version as the walk starts, for each list's first row: a judge
// of the recording names a tool that can differ from its own.
const tool = toolVersion(root);
let sender;
try {
  sender = senderOverride(process.env);
} catch (e) {
  console.error(e.message);
  process.exit(2);
}

// The run turns its switches off before the browser closes.
const browser = await launchBrowser(root);
// The rules for the video stream (dropped frames, requests for a keyframe)
// are for a software decoder.
const hardware = await hardwareVideoDecoders(browser);
if (hardware.length) {
  console.error(`the tool's browser decodes video in hardware (${hardware.map(decoder => decoder.profile).join(', ')}); ` +
    'the walk needs a browser that decodes in software, which --disable-accelerated-video-decode gives on a machine whose Chrome honours it');
  await browser.close();
  process.exit(2);
}
// For cloud the page goes to the tool's cloud, whose certificate the e2e
// test CA signed: the Mini's build trusts that CA, and this browser does not.
// takeover opens a second tab beside the page, in the page's context.
const context = await browser.newContext({ viewport: { width: 1920, height: 1080 }, ignoreHTTPSErrors: true });
const page = await context.newPage();
let created;
try {
  created = await createSwitches({
    page, url, remoteHost, firmwareFile: args.image, cloudKeysUrl: bench.JETKVM_MOCK_CLOUD_JWKS_URL,
    plainCloud: Boolean(args['plain-cloud']), sender, tailscaleKeyFile,
  });
} catch (e) {
  console.error(e.message);
  await browser.close();
  process.exit(2);
}
const { switches, host, hostExec, hostReports, showPage, storedSettings, close } = created;

// Every variable off. The other switches need the page's RPC channel, so
// video goes on first and off last. strict: stop at the first switch that
// fails; otherwise go on, so that each switch still gets its off().
async function allOff(strict) {
  const attempt = async step => {
    try {
      await step();
    } catch (e) {
      if (strict) throw e;
      console.error(`cleanup: ${e.message}`);
    }
  };
  await attempt(async () => {
    await switches.video.on();
    await until(switches.video.isOn, 60_000, `no video from ${url} after 60 s`);
  });
  for (const name of VARIABLES.filter(name => name !== 'video').reverse()) {
    await attempt(async () => {
      await switches[name].off();
      await until(async () => (await switches[name].isOn()) === false, 60_000, `${name} is still on after 60 s`);
    });
  }
  await attempt(() => switches.video.off());
}

let interrupted = false;
process.on('SIGINT', () => { interrupted = true; });
process.on('SIGTERM', () => { interrupted = true; });

// One list, from the clean state: 'pass', 'fail', 'undecided', or 'not run'.
async function run(name) {
  const list = lists[name];
  const directory = path.join(out, name);
  fs.mkdirSync(directory, { recursive: true });
  const recording = path.join(directory, 'recording.jsonl');
  let completed = false;
  let recorder = null;
  const beforeUpdates = [];
  console.log(`\nlist ${name}  target ${url}  output ${directory}`);
  try {
    await until(hostReports, 30_000, `${remoteHost} does not report its CD-ROM drive after 30 s`);
    // The Mini's stored settings that change what a list measures, each at
    // its default. It needs RPC, so video is on; a new bitrate can stop
    // video for a moment.
    await switches.video.on();
    await until(switches.video.isOn, 60_000, `no video from ${url} after 60 s`);
    // A list with a mount also starts with the Mini's drive as a CD-ROM
    // drive, which can make the Mini enumerate on USB again.
    const settings = await storedSettings({ drive: list.some(step => MOUNTS.some(mount => step.state[mount])) });
    await until(switches.video.isOn, 60_000, `no video from ${url} 60 s after the Mini's settings were set to their defaults`);
    // The host's screen shows the target page during every list.
    const shown = await showPage();
    // What the list's variables need from the bench.
    const facts = {};
    for (const variable of VARIABLES.filter(variable => list.some(step => step.state[variable]))) {
      Object.assign(facts, await switches[variable].prepare?.());
    }
    await allOff(true);
    const state = {};
    // This machine's network interface towards the Mini, whose state each sample has.
    const link = await toolLink(url);
    // The host's clock at the start of a list with the sender, from which
    // its log is read after the list.
    const senderLog = list.some(step => step.state.osServices) ? senderLogCommands(host) : null;
    const logSince = senderLog && Number((await hostExec(senderLog.now)).trim());
    // No full stats while the host reads the TF card's image (recorder.mjs).
    const withoutFullStats = now => MOUNTS.some(mount => mount.startsWith('tf') && now[mount]);
    // An update restarts the Mini, which sets its counts to zero: the
    // recorder reads them once more when the update begins.
    recorder = startRecorder(recording, { page, url, switches, state, link, withoutFullStats, beforeRestart: now => Boolean(now.ota) });
    // cores: of the machine that runs the tool, for the load in each sample.
    recorder.record({ run: { list: name, board, url, remoteHost, host, cores: os.cpus().length, toolInterface: link?.name, tool, settings, ...shown, ...facts } });
    // The Mini's tasks with the least free stack of each go into the
    // recording before a restart loses them: asked for at the start of each
    // update, inside its change, in which no rule judges video. The update
    // does not wait for the answer, so its time is the update's alone. The
    // end of the list has them once more (below).
    const driven = { ...switches, ota: { ...switches.ota, on: () => {
      beforeUpdates.push(taskStacksRow(url, 'before the update'));
      return switches.ota.on();
    } } };
    // The seconds of the recording that are not normal by the board's rules,
    // with the change whose switch is done but whose row is not written yet.
    // A new sample comes once per second, and the driver asks more often.
    // The keys are sent from this process every 100 ms, so the judging of a
    // sample must be short: it has the first row, for the host, and the
    // rows from SETTLE_LOOKS_BACK_MS before the change on. A breach that is
    // older than that is not the change's (settledAt() in driver.mjs).
    let judged = {};
    const secondsSoFar = pending => {
      if (judged.rows !== recorder.rows.length || judged.startedAt !== pending.startedAt) {
        const recent = recorder.rows.filter((row, index) => index === 0 || row.t >= pending.startedAt - SETTLE_LOOKS_BACK_MS);
        judged = { rows: recorder.rows.length, startedAt: pending.startedAt,
          seconds: secondsNotNormal([...recent, pending], criteria, known) };
      }
      return judged.seconds;
    };
    // The driver waits for a change's switch as long as the board's rules
    // give the change, and then for the new state to settle.
    completed = await drive(list, driven, state, row => {
      console.log(changeLine(row, changeLimit(criteria, [], row.change, row.state, host)));
      recorder.record(row);
    }, () => interrupted, (change, at) => changeLimit(criteria, known, change, at, host), secondsSoFar);
    // What the sender logged during the list that the OS Services protocol
    // does not allow the Mini to send: the judge fails the list on such a line.
    if (senderLog) {
      recorder.record({ senderLog: await hostExec(senderLog.log(logSince), 60_000).then(output => {
        const entries = senderLog.entries(output, logSince);
        return { since: logSince, lines: entries.length,
          protocolErrors: entries.filter(entry => SENDER_PROTOCOL_ERROR.test(entry.message)).map(entry => entry.message) };
      }, error => ({ since: logSince, error: error.message })) });
    }
  } catch (e) {
    console.error(`the run did not take place: ${e.message}`);
  } finally {
    // After the last sample and before the switches go off.
    await recorder?.stop(async () => [...await Promise.all(beforeUpdates), await taskStacksRow(url, 'end of the list')]);
    await allOff(false);
  }
  if (!recorder || interrupted) return 'not run';
  const rows = readRows(recording);
  const verdict = judge(rows, criteria, known);
  const text = report(verdict, rows[0].t) + (completed ? '' : '\nThe list ended at the failed change.');
  fs.writeFileSync(path.join(directory, 'report.txt'), text + '\n');
  console.log(text);
  return verdict.outcome;
}

// --install: the image onto a Mini that runs another build, by the
// installer of the integration tests (tools/integration/lib/install.mjs),
// before the page has a session. False when the Mini did not take it.
async function install() {
  const installer = spawn(process.execPath, [path.join(root, 'tools', 'integration', 'lib', 'install.mjs')],
    { stdio: 'inherit', env: { ...process.env, INTEGRATION_BENCH: benchFile, INTEGRATION_IMAGE: args.image } });
  return await new Promise(resolve => installer.on('close', status => resolve(status === 0)));
}

const results = {};
try {
  const ready = !args.install || await install();
  for (const name of names) {
    if (!ready) break;
    if (!interrupted) results[name] = await run(name);
  }
} finally {
  await close();
  await browser.close();
}
if (names.length > 1) {
  console.log('');
  for (const name of names) console.log(`${(results[name] ?? 'not run').toUpperCase().padEnd(12)} ${name}`);
}
const all = names.map(name => results[name] ?? 'not run');
const worst = status => all.some(result => EXIT[result] === status);
process.exit(all.includes('not run') ? 2 : worst(1) ? 1 : worst(3) ? 3 : 0);
