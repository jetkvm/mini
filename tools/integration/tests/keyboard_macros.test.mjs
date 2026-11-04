import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect } from '../lib/mini.mjs';

// The Mini stores up to 25 macros of up to 10 steps with up to 10 keys each.
const MAX_MACROS = 25;
const MAX_STEPS = 10;
// The ten longest key names of the page's keyboard, and every modifier.
const LONGEST_KEY_NAMES = [
  'NumpadMemorySubtract', 'NumpadMemoryMultiply', 'ThousandsSeparator', 'NumpadMemoryRecall', 'NumpadMemoryDivide',
  'NumpadMemoryStore', 'NumpadMemoryClear', 'NumpadHexadecimal', 'NumpadGreaterThan', 'NumpadExclamation',
];
const MODIFIERS = ['ControlLeft', 'ControlRight', 'ShiftLeft', 'ShiftRight', 'AltLeft', 'AltRight', 'MetaLeft', 'MetaRight'];

// A list at every limit: ids of 31 bytes, and names of 50 characters, 47 of
// which take three bytes each in UTF-8.
const fullList = marker => Array.from({ length: MAX_MACROS }, (_, index) => {
  const number = String(index).padStart(2, '0');
  const prefix = `integration-lim-${marker}-${number}-`;
  return {
    id: prefix.padEnd(31, 'x'),
    name: '界'.repeat(47) + number + marker.toUpperCase(),
    sortOrder: index,
    steps: Array.from({ length: MAX_STEPS }, () => ({ keys: [...LONGEST_KEY_NAMES], modifiers: [...MODIFIERS], delay: 2000 })),
  };
});

// connect() records the Mini's macros, and close() stores them again.
test('25 macros of 10 steps with 10 keys come back as stored, and a second full list replaces the first', { timeout: 120_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  const startedAt = Date.now();
  for (const marker of ['a', 'b']) {
    const macros = fullList(marker);
    await mini.rpc('setKeyboardMacros', { params: { macros } });
    assert.deepEqual(await mini.rpc('getKeyboardMacros'), macros, `list ${marker} comes back as stored`);
  }
  assert.ok(Date.now() - startedAt <= 60_000, 'the test takes 60 seconds at most');
});
