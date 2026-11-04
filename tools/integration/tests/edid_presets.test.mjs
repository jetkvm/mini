import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { connect } from '../lib/mini.mjs';

const GENERATOR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'edid');

// The bytes of one preset of tools/edid/generate_presets.py, as upper-case hex.
const generated = identity => execFileSync('python3', ['-c',
  'import sys; sys.path.insert(0, sys.argv[1]); from generate_presets import preset; print(preset(sys.argv[2]).hex().upper())',
  GENERATOR, identity], { encoding: 'utf8' }).trim();

// Each preset lists only modes that the Mini captures, and the host picks its
// resolution from them. The Mini takes no other EDID.
test('the Mini offers exactly its generated EDID presets, uses one of them and takes no custom EDID', { timeout: 60_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  const expected = [
    { name: 'JetKVM Default', edid: generated('default') },
    { name: 'DELL D2721H, 1920x1080', edid: generated('dell_d2721h') },
  ];
  const presets = await mini.rpc('getEDIDPresets');
  assert.deepEqual(presets.map(({ name, edid }) => ({ name, edid: edid.toUpperCase() })), expected, 'the Mini\'s presets');
  const inUse = (await mini.rpc('getEDID')).toUpperCase();
  assert.ok(expected.some(preset => preset.edid === inUse), `the EDID in use is a preset: ${inUse}`);
  assert.ok(!(await mini.rpc('getDeviceCapabilities')).includes('custom_edid'), 'the Mini reports no custom EDID');
});
