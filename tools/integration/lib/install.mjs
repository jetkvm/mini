#!/usr/bin/env node
// Installs INTEGRATION_IMAGE on the bench's Mini unless it already runs it.
// Flashes over the debugger (tools/e2e/serial_flash.sh) when the bench file
// names the debugger's MAC in a comment; otherwise installs it as an update
// from the stand-in update server.
import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { benchFile } from '../../combined_load/runner/build.mjs';
import { connect, until } from './mini.mjs';
import { updateServer } from './servers.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');

function overDebugger(bench, remoteHost) {
  const mac = fs.readFileSync(bench, 'utf8').split('\n')
    .some(line => line.startsWith('#') && /([0-9a-f]{2}:){5}[0-9a-f]{2}/i.test(line));
  if (!mac || !remoteHost) return false;
  const system = execFileSync('ssh', [remoteHost, 'uname -s'], { encoding: 'utf8', timeout: 30_000 }).trim();
  return system === 'Linux' || system === 'Darwin' || /^(MINGW|MSYS)/.test(system);
}

const mini = await connect();
let server;
try {
  const running = (await mini.status()).firmware;
  if (running.elfSha256 !== mini.image.elfSha256) {
    const bench = benchFile(root, process.env.INTEGRATION_BENCH);
    const flash = overDebugger(bench, mini.remoteHost);
    console.log(`the Mini runs ${running.version}; installing ${mini.image.version} ${flash ? 'over the debugger' : 'as an update'}`);
    const startedAt = Date.now();
    if (flash) {
      const written = spawnSync(path.join(root, 'tools', 'e2e', 'serial_flash.sh'),
        [bench, path.dirname(process.env.INTEGRATION_IMAGE)], { stdio: ['ignore', 'inherit', 'inherit'] });
      if (written.status !== 0) throw new Error('tools/e2e/serial_flash.sh failed');
    } else {
      server = await updateServer(mini);
      await mini.rpc('setUpdateApiUrl', { url: server.base });
      // A pinned version is exempt from the signature requirement and from the
      // newer-version rule.
      // The Mini may restart before answering.
      mini.rpc('tryUpdateComponents',
        { params: { components: { system: '99.0.0' } }, includePreRelease: false, resetConfig: false }, 30_000).catch(() => undefined);
    }
    await until(() => mini.restarted(startedAt), restarted => restarted, 180);
    const now = (await mini.status()).firmware;
    if (now.elfSha256 !== mini.image.elfSha256) throw new Error(`the Mini runs ${now.version} after the install, and not the image`);
    console.log(`installed ${mini.image.version} in ${Math.round((Date.now() - startedAt) / 1000)} s`);
  }
} catch (e) {
  console.error(`the image was not installed: ${e.message}`);
  process.exitCode = 2;
} finally {
  // Otherwise the next connect() records the stand-in server URL as found.
  await mini.close().catch(e => {
    console.error(`the settings of the Mini were not stored again: ${e.message}`);
    process.exitCode = 2;
  });
  server?.close();
}
