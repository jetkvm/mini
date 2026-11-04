import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hostAgent } from '../lib/host.mjs';
import { connect, until } from '../lib/mini.mjs';
import { hostTestUsb } from '../lib/usb.mjs';

// Linux input codes of the host's events.
const REL_X = 0;
const REL_Y = 1;
// Every second, after one second without input, with no random delay.
const JIGGLER = { inactivity_limit_seconds: 1, jitter_percentage: 0, schedule_cron_tab: '* * * * * *', timezone: 'UTC' };

// A Linux host. Without a session only the jiggler moves the Mini's relative
// mouse. connect() records the jiggler and its schedule, and close() stores
// them again; the USB state is stored again as found before that.
test('the jiggler moves the host\'s mouse on both axes while the Mini has no session', { timeout: 120_000 }, async t => {
  const host = await hostAgent(t);
  if (!host) return;
  const mini = await connect();
  let restoreUsb = null;
  t.after(async () => {
    try {
      await restoreUsb?.();
    } finally {
      await mini.close();
    }
  });
  restoreUsb = await hostTestUsb(mini);
  await host.inputDevices(['keyboard', 'absolute_mouse', 'relative_mouse'], 30);

  const startedAt = Date.now();
  await mini.rpc('setJigglerConfig', { jigglerConfig: JIGGLER });
  await mini.rpc('setJigglerState', { enabled: true });
  assert.deepEqual(await mini.rpc('getJigglerConfig'), JIGGLER);
  assert.equal(await mini.rpc('getJigglerState'), true);

  await mini.leave();
  await host.clear('/events/mouse');
  const clearedAt = Date.now();
  const axes = new Set();
  await until(() => host.get('/events/mouse'), events => {
    for (const event of events) if (event.type === 'mouse_move_rel' && event.value !== 0) axes.add(event.code);
    return axes.has(REL_X) && axes.has(REL_Y);
  }, 15).catch(() => undefined);
  t.diagnostic(`relative moves on the axes ${[...axes].sort().join(' and ') || 'none'} after ${Date.now() - clearedAt} ms`);
  assert.ok(axes.has(REL_X) && axes.has(REL_Y), 'the host gets relative moves on X and on Y within 15 seconds');
  assert.ok(Date.now() - startedAt <= 60_000, 'the test takes 60 seconds at most');
});
