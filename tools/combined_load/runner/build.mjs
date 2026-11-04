#!/usr/bin/env node
// Builds the checkout's Dockerfile for a bench, on JETKVM_BUILD_HOST if the
// bench file sets it, otherwise with this machine's docker buildx.
//
//   node tools/combined_load/runner/build.mjs <bench> <directory> [--version=<version>] [--plain-cloud]
//       [--usb-debugging-off]
//
// Prints the path of <directory>/jetkvm-mini.bin; Docker's output goes to
// stderr. The flasher files land beside it for tools/e2e/serial_flash.sh.
// USB debugging defaults on because the bench flashes and reads the console
// over USB Serial/JTAG; --usb-debugging-off builds it off, as a release does.
import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// The size that a build host's cache is cut down to after a build. Docker
// reads it as 150 GiB.
const CACHE_LIMIT = '150GB';

// The bench file of a bench name or a path, in the checkout at root.
export function benchFile(root, bench) {
  return fs.existsSync(bench) ? bench : path.join(root, 'tools', 'benches', `${bench}.env`);
}

// A bench file's values: lines NAME=value, the value as it stands or in one
// pair of quotes, as the shell of tools/e2e/run.sh reads such a line.
export function readBench(file) {
  return Object.fromEntries(fs.readFileSync(file, 'utf8').split('\n')
    .map(line => line.match(/^([A-Z0-9_]+)=(.*)$/)).filter(Boolean)
    .map(m => [m[1], m[2].replace(/^(["'])(.*)\1$/, '$2')]));
}

// The Dockerfile's build arguments for a bench file's values: the board of
// JETKVM_SKU, the fixed BLE setup code for a bench with JETKVM_WIFI_SSID, the
// cloud's key URL over https unless the cloud is plain, and USB debugging on
// unless usbDebugging is false.
export function benchBuildArguments(bench, plainCloud, usbDebugging = true) {
  const board = { 'jetkvm-mini-ethernet': 'ethernet', 'jetkvm-mini-wireless': 'wifi' }[bench.JETKVM_SKU];
  if (!board) throw new Error(`JETKVM_SKU "${bench.JETKVM_SKU}" is not jetkvm-mini-ethernet or jetkvm-mini-wireless`);
  const keys = bench.JETKVM_MOCK_CLOUD_JWKS_URL ?? '';
  return {
    BOARD: board,
    WIFI_SETUP_LOG_CODE: bench.JETKVM_WIFI_SSID ? '1' : '0',
    MOCK_CLOUD_JWKS_URL: plainCloud ? keys : keys.replace(/^http:/, 'https:'),
    USB_DEBUGGING: usbDebugging ? '1' : '0',
  };
}

// The build's output directory as a path in the checkout at root, with /, or
// undefined for a directory outside the checkout. The files of earlier builds
// and runs in it are not source: they do not count as changes and are not
// sent to the build.
export function outputInCheckout(root, directory) {
  const relative = path.relative(root, path.resolve(directory));
  if (!relative || relative.startsWith('..') || path.isAbsolute(relative)) return undefined;
  return relative.split(path.sep).join('/');
}

// A prerelease of the latest release tag, so the Mini is offered the real
// release afterwards: g and five digits of the commit; with uncommitted
// changes outside the output directory, e and five base-36 digits of the
// time, so that each such build is installed.
export function benchVersion(root, directory) {
  const git = (...args) => execFileSync('git', ['-C', root, ...args], { encoding: 'utf8' }).trim();
  const latest = git('tag', '-l', 'release/*').split('\n').map(tag => tag.replace(/^release\//, ''))
    .filter(tag => /^\d+\.\d+\.\d+$/.test(tag))
    .sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop();
  if (!latest) throw new Error('no release/X.Y.Z tag to count from');
  const output = outputInCheckout(root, directory);
  return git('status', '--porcelain', '--', '.', ...(output ? [`:(exclude,literal)${output}`] : []))
    ? `${latest}-e${Math.floor(Date.now() / 1000).toString(36).slice(-5)}`
    : `${latest}-g${git('rev-parse', 'HEAD').slice(0, 5)}`;
}

// Docker's output goes to stderr: the stdout of a command that builds is the
// path of what it built.
const run = (args, stdio = ['ignore', 2, 2]) => spawnSync('docker', args, { stdio }).status === 0;

// The buildx builder of a bench file's JETKVM_BUILD_HOST, made when it does
// not exist yet, or undefined without a build host: then the builder that
// docker buildx has selected on this machine builds.
export function benchBuilder(bench) {
  // user@host, reached over SSH, with Docker. Its builder is a BuildKit
  // container in that machine's Docker, which keeps the layers and the cache
  // mounts.
  // Or tcp://host:port, a BuildKit service that accepts only clients with its
  // certificate. Its builder uses buildx's remote driver with the client
  // files in ~/.docker/buildx/certs/<builder>/.
  const host = bench.JETKVM_BUILD_HOST;
  if (!host) return undefined;
  const builder = `jetkvm-${host.replace(/[^A-Za-z0-9]+/g, '-')}`;
  let driver = ['--driver', 'docker-container', `ssh://${host}`];
  if (host.startsWith('tcp://')) {
    const certs = path.join(os.homedir(), '.docker', 'buildx', 'certs', builder);
    const files = { cacert: 'ca.pem', cert: 'client.pem', key: 'client-key.pem' };
    if (Object.values(files).some(file => !fs.existsSync(path.join(certs, file)))) {
      throw new Error(`the client files of ${host} are not all in ${certs}: ${Object.values(files).join(', ')}`);
    }
    driver = ['--driver', 'remote', '--driver-opt',
      Object.entries(files).map(([name, file]) => `${name}=${path.join(certs, file)}`).join(','), host];
  }
  // Builds of several benches can start together: a create that fails is
  // no fault when another build has made the builder in the meantime.
  const exists = () => run(['buildx', 'inspect', builder], 'ignore');
  if (!exists() && !run(['buildx', 'create', '--name', builder, ...driver]) && !exists()) {
    throw new Error('docker buildx create failed');
  }
  return builder;
}

// Builds the checkout at root for the bench and returns the image file, in
// directory. version takes the place of the prerelease of the latest release.
export function buildImage(root, bench, { directory, plainCloud = false, usbDebugging = true, version }) {
  const docker = args => {
    if (!run(args)) throw new Error(`docker ${args.slice(0, 2).join(' ')} failed`);
  };
  const host = bench.JETKVM_BUILD_HOST;
  const builder = benchBuilder(bench);
  const values = { VERSION: version ?? benchVersion(root, directory), ...benchBuildArguments(bench, plainCloud, usbDebugging) };
  console.error(`building ${values.VERSION} (${values.BOARD}) on ${host ?? 'this machine'}`);
  // An output directory in the checkout stays out of the context: the build
  // reads a copy of the Dockerfile, and Dockerfile.dockerignore beside the
  // copy takes the place of .dockerignore and names that directory too.
  const output = outputInCheckout(root, directory);
  const copy = output && fs.mkdtempSync(path.join(os.tmpdir(), 'jetkvm-build-'));
  try {
    if (copy) {
      fs.copyFileSync(path.join(root, 'Dockerfile'), path.join(copy, 'Dockerfile'));
      fs.writeFileSync(path.join(copy, 'Dockerfile.dockerignore'),
        `${fs.readFileSync(path.join(root, '.dockerignore'), 'utf8')}\n/${output}\n`);
    }
    docker(['buildx', 'build', ...(builder ? ['--builder', builder] : []),
      ...(copy ? ['--file', path.join(copy, 'Dockerfile')] : []),
      ...Object.entries(values).flatMap(([name, value]) => ['--build-arg', `${name}=${value}`]),
      '--output', `type=local,dest=${directory}`, root]);
  } finally {
    if (copy) fs.rmSync(copy, { recursive: true, force: true });
  }
  // The builder removes what it used longest ago first, so this build's
  // layers and cache mounts stay. The cache of this machine's Docker is not
  // the tool's to cut down.
  if (builder && !run(['buildx', 'prune', '--builder', builder, '--max-used-space', CACHE_LIMIT, '--force'], 'ignore')) {
    console.error(`the cache of the builder ${builder} is not cut down to 150 GiB: docker buildx prune failed`);
  }
  return path.join(directory, 'jetkvm-mini.bin');
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [bench, directory, ...options] = process.argv.slice(2);
  const args = Object.fromEntries(options.map(option => {
    const [name, ...value] = option.replace(/^--/, '').split('=');
    return [name, value.join('=') || true];
  }));
  if (!bench || !directory || directory.startsWith('--') || Object.keys(args).some(name => !['version', 'plain-cloud', 'usb-debugging-off'].includes(name)) || args.version === true) {
    console.error('usage: build.mjs <bench> <directory> [--version=<version>] [--plain-cloud] [--usb-debugging-off]');
    process.exit(2);
  }
  const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
  try {
    console.log(buildImage(root, readBench(benchFile(root, bench)),
      { directory: path.resolve(directory), plainCloud: Boolean(args['plain-cloud']),
        usbDebugging: !args['usb-debugging-off'], version: args.version }));
  } catch (e) {
    console.error(`no image: ${e.message}`);
    process.exit(1);
  }
}
