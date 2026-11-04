// The bench's host (the computer on the Mini's USB and HDMI): the remote agent
// (frontend/e2e/remote-agent) on port 9182, Linux only, and SSH commands.
import { execFile, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { benchFile, readBench } from '../../combined_load/runner/build.mjs';
import { blockChecksums } from '../../combined_load/runner/content.mjs';
import { hostFor, quote, READER_REPORT } from '../../combined_load/runner/hosts.mjs';
import { SSH_OPTIONS } from '../../combined_load/runner/switches.mjs';
import { until } from './mini.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const AGENT_PORT = 9182;
const AGENT_SOURCE = path.join(root, 'frontend', 'e2e', 'remote-agent');
const run = promisify(execFile);

async function startAgent(remoteHost, healthy) {
  await run('go', ['build', '-buildvcs=false', '-o', 'remote-agent', '.'],
    { cwd: AGENT_SOURCE, env: { ...process.env, GOOS: 'linux', GOARCH: 'amd64' }, timeout: 120_000 });
  const binary = path.join(AGENT_SOURCE, 'remote-agent');
  const sum = createHash('sha256').update(readFileSync(binary)).digest('hex');
  // POSIX sh; the host's login shell may differ.
  const onHost = async command => (await run('ssh', [...SSH_OPTIONS, remoteHost, `sh -c ${quote(command)}`], { timeout: 30_000 })).stdout;
  if ((await onHost('cat /tmp/remote-agent.sha256 2>/dev/null || true')).trim() === sum && await healthy()) return;
  await onHost('sudo -n true && { sudo -n pkill -x remote-agent 2>/dev/null || test $? = 1; } && sleep 0.5');
  await run('scp', [...SSH_OPTIONS, binary, `${remoteHost}:/tmp/remote-agent`], { timeout: 60_000 });
  await onHost(`nohup sudo -n PORT=${AGENT_PORT} /tmp/remote-agent </dev/null >/tmp/remote-agent.log 2>&1 & sleep 0.5`);
  await onHost(`printf %s ${sum} > /tmp/remote-agent.sha256`);
  await until(healthy, answers => answers, 10).catch(() => {
    throw new Error(`the remote agent did not start on ${remoteHost} (/tmp/remote-agent.log there)`);
  });
}

/**
 * The remote agent on the bench's host, (re)deployed if needed. Skips `t` and
 * returns null on a non-Linux host. Returns { get, clear, inputDevices,
 * answers, exec }.
 */
export async function hostAgent(t) {
  const remoteHost = readBench(benchFile(root, process.env.INTEGRATION_BENCH)).JETKVM_REMOTE_HOST;
  if (!remoteHost) {
    t.skip('needs a Linux host with the remote agent; the bench file names no host for the e2e suite (JETKVM_REMOTE_HOST)');
    return null;
  }
  const { stdout } = await run('ssh', [...SSH_OPTIONS, remoteHost, 'uname -s'], { timeout: 30_000 });
  const { system } = hostFor(stdout);
  if (system !== 'linux') {
    t.skip(`needs a Linux host with the remote agent; the host of this bench is ${system}`);
    return null;
  }
  const base = `http://${remoteHost.split('@').pop()}:${AGENT_PORT}`;
  const request = async (method, route, timeoutMs = 5000) => {
    const response = await fetch(base + route, { method, signal: AbortSignal.timeout(timeoutMs) });
    if (!response.ok) throw new Error(`remote agent ${route}: ${response.status} ${await response.text()}`);
    return response.json();
  };
  const get = route => request('GET', route);
  const clear = route => request('DELETE', route);
  const answers = () => request('GET', '/health', 2000).then(health => health.status === 'ok', () => false);
  const exec = async (command, timeoutMs = 15_000) => {
    try {
      return (await run('ssh', [...SSH_OPTIONS, remoteHost, `sh -c ${quote(command)}`], { timeout: timeoutMs })).stdout;
    } catch (e) {
      throw new Error(`${remoteHost}: ${e.stderr?.trim() || e.message}`);
    }
  };
  await startAgent(remoteHost, answers);
  const inputDevices = (types, seconds) => until(
    async () => (await get('/input/devices')).filter(device => device.is_jetkvm).map(device => device.type).sort(),
    found => JSON.stringify(found) === JSON.stringify([...types].sort()), seconds);
  return { get, clear, inputDevices, answers, exec };
}

/**
 * SSH to the bench's host: { login, system, commands (hosts.mjs), exec, stream }.
 * exec errors carry the exit `status` (255: ssh did not connect). A streamed
 * command must print periodically to notice stop().
 */
export async function connectHost(mini) {
  const login = mini.remoteHost;
  if (!login) throw new Error('the bench file has no JETKVM_REMOTE_HOST and no JETKVM_COMBINED_LOAD_HOST');
  // POSIX sh; the host's login shell may differ.
  const onHost = command => [...SSH_OPTIONS, login, `sh -c ${quote(command)}`];
  const exec = (command, { input = '', timeoutMs = 30_000 } = {}) => new Promise((resolve, reject) => {
    const child = spawn('ssh', onHost(command), { stdio: ['pipe', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      reject(new Error(`${login}: no end after ${timeoutMs} ms: ${command}`));
    }, timeoutMs);
    child.stdout.setEncoding('utf8').on('data', chunk => { stdout += chunk; });
    child.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; });
    child.on('error', error => {
      clearTimeout(timer);
      reject(error);
    });
    child.on('close', code => {
      clearTimeout(timer);
      if (code === 0) resolve(stdout);
      else reject(Object.assign(new Error(`${login}: ${stderr.trim() || `exit ${code}`}: ${command}`), { status: code }));
    });
    child.stdin.on('error', () => undefined);
    child.stdin.end(input);
  });
  const stream = (command, onLine) => {
    const child = spawn('ssh', onHost(command), { stdio: ['ignore', 'pipe', 'pipe'] });
    let pending = '';
    let stderr = '';
    child.stdout.setEncoding('utf8').on('data', chunk => {
      const lines = (pending + chunk).split('\n');
      pending = lines.pop();
      lines.forEach(onLine);
    });
    child.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; });
    const ended = new Promise(resolve => child.on('close', () => resolve(stderr.trim())));
    return { ended, stop: () => { child.kill(); return ended; } };
  };
  const commands = hostFor(await exec('uname -s'));
  return { login, system: commands.system, commands, exec, stream };
}

// macOS keeps an "unreadable disk" dialog after the Mini unmounts an image.
// Call after unmounting; no-op on other hosts.
export async function closeDiskDialogs(mini) {
  if (!mini.remoteHost) return;
  const host = await connectHost(mini);
  if (host.commands.closeDiskDialogs) await host.exec(host.commands.closeDiskDialogs);
}

/**
 * The host display the Mini captures: { found, set(mode), restore() }. Needs a
 * Linux GNOME (Mutter) host with one display (displayMode in hosts.mjs);
 * otherwise skips `t` and returns null. Modes are not persisted on the host.
 */
export async function hostDisplay(t, mini) {
  const host = await connectHost(mini);
  if (host.system !== 'linux') {
    t.skip(`needs a Linux host, whose display mode the walk's host code sets; the host of this bench is ${host.system}`);
    return null;
  }
  const set = async mode => {
    const output = await host.exec(host.commands.displayMode(mode), { timeoutMs: 60_000 });
    const [width, height, hz] = output.trim().split(/\s+/).map(Number);
    if (!(width > 0 && height > 0 && hz > 0)) throw new Error(`${host.login} did not print its display mode: "${output.trim()}"`);
    return { width, height, hz };
  };
  // A command that fails on the host is a bench limitation (skip); ssh
  // failures and timeouts fail the test.
  let found;
  try {
    found = await set();
  } catch (error) {
    if (typeof error.status !== 'number' || error.status === 255) throw error;
    t.skip(`needs a Linux host whose display mode Mutter sets: ${error.message}`);
    return null;
  }
  const restore = async () => {
    const now = await set();
    if (now.width !== found.width || now.height !== found.height || now.hz !== found.hz) await set(found);
  };
  return { found, set, restore };
}

// Answers NTP requests from argv[1] only and prints the answered count.
const NTP_RESPONDER = `
import select, socket, struct, sys, time
source = sys.argv[1]
server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("0.0.0.0", 123))
def stamp(seconds):
    seconds += 2208988800
    return struct.pack("!II", int(seconds), int((seconds % 1) * 4294967296))
count = 0
while True:
    print(count, flush=True)
    if not select.select([server], [], [], 1)[0]:
        continue
    request, peer = server.recvfrom(512)
    if peer[0] != source or len(request) < 48:
        continue
    now = stamp(time.time())
    version = (request[0] >> 3) & 7
    # Leap 0, the request's version, mode 4 (server); stratum 1; the request's
    # transmit time as the origin time.
    server.sendto(struct.pack("!BBBb", version << 3 | 4, 1, 0, -20) + bytes(8) + b"LOCL" + now + request[40:48] + now + now, peer)
    count += 1
`;

// NTP server on the host answering `source` only: { count(), stop() }. count()
// throws if the server died. Linux needs passwordless sudo for port 123.
export async function ntpResponder(host, source) {
  const python = { linux: 'sudo -n python3', macos: 'python3' }[host.system];
  if (!python) throw new Error(`no NTP server on a ${host.system} host: this needs a Linux or macOS host`);
  let requests = null;
  let stopped = false;
  let failure = null;
  const server = host.stream(`${python} -u -c ${quote(NTP_RESPONDER)} ${quote(source)}`, line => { requests = Number(line); });
  server.ended.then(stderr => {
    if (!stopped) failure = new Error(`${host.login}: the NTP server ended: ${stderr || 'no error output'}`);
  });
  const count = () => {
    if (failure) throw failure;
    return requests;
  };
  const end = Date.now() + 15_000;
  while (count() === null) {
    if (Date.now() > end) {
      stopped = true;
      await server.stop();
      throw new Error(`${host.login}: the NTP server did not start in 15 s`);
    }
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  return { count, stop: () => { stopped = true; return server.stop(); } };
}

/**
 * Mounts a TF card file as a disk and has the host read every 1 MiB block,
 * comparing each with `image`. Returns { blocksWrong, readErrors } and
 * unmounts. Needs nothing mounted and a Linux or Windows host; leaves the
 * Mini's drive in disk mode.
 */
export async function hostReadsImage(mini, host, filename, image, seconds = 120) {
  const { commands } = host;
  if (commands.releaseMedia) throw new Error(`${host.login} is a ${host.system} host, which keeps a disk that the Mini unmounts`);
  const sums = blockChecksums(image);
  if (commands.canRead) await host.exec(commands.canRead);
  await host.exec(`(${commands.stopReader}); ${commands.expectBlocks(sums, { firstReads: true, forget: true })}`);
  const report = async () => {
    const [read = '', counts = ''] = (await host.exec(READER_REPORT)).split('\n');
    const [checked = 0, wrongFirst = 0, wrongLater = 0] = counts.trim().split(/\s+/).filter(Boolean).map(Number);
    return { read: new Set(read.trim().split(/\s+/).filter(Boolean).map(Number)), checked, blocksWrong: wrongFirst + wrongLater };
  };
  // medium(): null when the host sees none, undefined when the watch is stale.
  let latest;
  let at = 0;
  const watch = host.stream(commands.watchMedia, line => {
    const fields = line.trim().split(/\s+/);
    latest = fields[0] === 'none' ? null : commands.medium(fields);
    at = Date.now();
  });
  const medium = () => (Date.now() - at < 3000 ? latest : undefined);
  try {
    await mini.rpc('mountWithStorage', { filename, mode: 'Disk' }, 60_000);
    await until(medium, now => now?.type === 'disk', 60);
    // udev's ATA IDENTIFY probe is refused and counted as a read error, so
    // the baseline is taken after udev settles.
    let settledAt = 0;
    if (host.system === 'linux') {
      await host.exec('udevadm settle --timeout=10 || true');
      settledAt = Date.now();
    }
    const mounted = await until(medium, now => now?.type === 'disk' && at > settledAt, 30);
    await host.exec(commands.startReader);
    const covered = await until(report, now => sums.every((sum, block) => now.read.has(block)), seconds);
    // The reader notes a block before saving its result; wait for one more.
    await until(report, now => now.checked > covered.checked, 30);
    // Read errors are sampled before stopReader aborts an in-flight read.
    const read = await until(medium, now => now?.type === 'disk', 30);
    await host.exec(commands.stopReader);
    const { blocksWrong } = await report();
    return { blocksWrong, readErrors: (read.readErrors ?? 0) - (mounted.readErrors ?? 0) };
  } finally {
    await watch.stop();
    await host.exec(commands.stopReader).catch(() => undefined);
    await mini.rpc('unmountImage', {}, 30_000).catch(() => undefined);
  }
}
