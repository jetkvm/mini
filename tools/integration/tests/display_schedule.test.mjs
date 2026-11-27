import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';
import { webrtcStats } from '../lib/video.mjs';

// A zone whose local time is 5 h 45 min from UTC, so that a window of two
// hours has the present time by Kathmandu's clock and not by UTC's.
const ZONE = 'Asia/Kathmandu';
const BRIGHTNESS = 64;

// "HH:MM" in a zone, `minutes` from now.
function timeIn(zone, minutes) {
  const parts = new Intl.DateTimeFormat('en-GB', { timeZone: zone, hour: '2-digit', minute: '2-digit', hourCycle: 'h23' })
    .formatToParts(new Date(Date.now() + minutes * 60_000));
  const part = type => parts.find(each => each.type === type).value;
  return `${part('hour')}:${part('minute')}`;
}

// The dim window and the off window of the local display: the backlight
// follows each window by the local time of the schedule's zone, the darker
// window wins, a stored schedule is in use again after a restart, and the
// settings end as found.
test('the display is dimmed and off in its two windows by local time, also after a restart', { timeout: 420_000 }, async t => {
  const mini = await connect();
  let found = null;
  t.after(async () => {
    try {
      if (!found) return;
      await mini.open();
      await mini.rpc('setDisplaySchedule', { params: found.schedule });
      await mini.rpc('setBacklightSettings', { params: found.backlight });
      assert.deepEqual(await mini.rpc('getDisplaySchedule'), found.schedule, 'the Mini stores the schedule as found');
      assert.deepEqual(await mini.rpc('getBacklightSettings'), found.backlight, 'the Mini stores the backlight as found');
    } finally {
      await mini.close();
    }
  });
  assert.ok((await mini.rpc('getDeviceCapabilities')).includes('display_schedule'), 'the Mini reports display_schedule');
  const zones = await mini.rpc('getDisplayScheduleTimezones');
  assert.ok(zones.includes(ZONE) && zones.includes('UTC') && zones.length > 300, `the Mini lists its zones (${zones.length})`);
  found = { schedule: await mini.rpc('getDisplaySchedule'), backlight: await mini.rpc('getBacklightSettings') };
  t.diagnostic(`schedule as found: ${JSON.stringify(found.schedule)}`);
  const brightness = async () => (await webrtcStats(mini)).localDisplay.brightness;
  // One hour to each side of now covers a difference between the two clocks.
  const now = { enabled: true, start: timeIn(ZONE, -60), end: timeIn(ZONE, 60) };
  const later = { enabled: true, start: timeIn(ZONE, 120), end: timeIn(ZONE, 180) };
  const never = window => ({ ...window, enabled: false });
  const store = (dim, off, timezone = ZONE) => mini.rpc('setDisplaySchedule', { params: { dim, off, timezone } });
  t.diagnostic(`a window of ${now.start} to ${now.end} in ${ZONE}; UTC is ${timeIn('UTC', 0)}`);

  // No idle timer, so only the windows change the backlight. The change
  // counts as a press: it lights the display for 30 s, also in a window.
  await store(never(now), never(now));
  await mini.rpc('setBacklightSettings', { params: { max_brightness: BRIGHTNESS, dim_after: 0, off_after: 0 } });
  await until(brightness, value => value === BRIGHTNESS, 10);

  await store(never(now), now);
  assert.deepEqual(await mini.rpc('getDisplaySchedule'), { dim: never(now), off: now, timezone: ZONE });
  await until(brightness, value => value === 0, 45);
  // A session that ends and starts is a state change; it does not wake the
  // display in a window.
  await mini.leave();
  await mini.open();
  assert.equal(await brightness(), 0, 'a new session did not wake the display in its off window');

  // The dim window alone, then both at once: the darker one wins. With the
  // off window at another time of the day, the dim window decides again.
  await store(now, never(now));
  await until(brightness, value => value === BRIGHTNESS / 2, 10);
  await store(now, now);
  await until(brightness, value => value === 0, 10);
  await store(now, later);
  await until(brightness, value => value === BRIGHTNESS / 2, 10);
  assert.deepEqual(await mini.rpc('getDisplaySchedule'), { dim: now, off: later, timezone: ZONE });

  // The same times of day in UTC are not now, so the zone decides.
  await store(now, now, 'UTC');
  await until(brightness, value => value === BRIGHTNESS, 10);

  // What the Mini cannot use, it refuses and does not store.
  await assert.rejects(store(now, now, 'Europe/Atlantis'));
  await assert.rejects(store({ ...now, end: now.start }, now));
  await assert.rejects(store(now, { ...never(now), end: now.start }));
  await assert.rejects(store(now, never(now), ''));
  assert.deepEqual(await mini.rpc('getDisplaySchedule'), { dim: now, off: now, timezone: 'UTC' });

  // A stored schedule is in use after a restart, once the Mini has the time.
  await store(later, now);
  const startedAt = Date.now();
  mini.rpc('reboot', { force: true }, 5000).catch(() => undefined);
  await until(() => mini.restarted(startedAt), restarted => restarted, 120);
  await mini.open();
  assert.deepEqual(await mini.rpc('getDisplaySchedule'), { dim: later, off: now, timezone: ZONE });
  await until(brightness, value => value === 0, 60);

  await store(never(later), never(now));
  await until(brightness, value => value === BRIGHTNESS, 10);
});
