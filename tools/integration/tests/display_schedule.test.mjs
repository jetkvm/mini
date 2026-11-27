import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';
import { webrtcStats } from '../lib/video.mjs';

// A zone whose local time is 5 h 45 min from UTC, so that a schedule of two
// hours is in its quiet hours by Kathmandu's clock and not by UTC's.
const ZONE = 'Asia/Kathmandu';
const BRIGHTNESS = 64;

// "HH:MM" in a zone, `minutes` from now.
function timeIn(zone, minutes) {
  const parts = new Intl.DateTimeFormat('en-GB', { timeZone: zone, hour: '2-digit', minute: '2-digit', hourCycle: 'h23' })
    .formatToParts(new Date(Date.now() + minutes * 60_000));
  const part = type => parts.find(each => each.type === type).value;
  return `${part('hour')}:${part('minute')}`;
}

// The quiet hours of the local display: the backlight follows the schedule by
// the local time of the schedule's zone, a stored schedule is in use again
// after a restart, and the settings end as found.
test('the display is off or dimmed in its quiet hours by local time, also after a restart', { timeout: 420_000 }, async t => {
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
  const brightness = async () => (await webrtcStats(mini)).localDisplay.brightness;
  const store = schedule => mini.rpc('setDisplaySchedule', { params: schedule });

  // No idle timer, so only the schedule changes the backlight. The change
  // counts as a press: it lights the display for 30 s, also in quiet hours.
  await store({ ...found.schedule, enabled: false });
  await mini.rpc('setBacklightSettings', { params: { max_brightness: BRIGHTNESS, dim_after: 0, off_after: 0 } });
  await until(brightness, value => value === BRIGHTNESS, 10);

  // One hour to each side of now covers a difference between the two clocks.
  const around = { start: timeIn(ZONE, -60), end: timeIn(ZONE, 60), timezone: ZONE };
  t.diagnostic(`quiet hours ${around.start} to ${around.end} in ${ZONE}; UTC is ${timeIn('UTC', 0)}`);
  await store({ enabled: true, action: 'off', ...around });
  assert.deepEqual(await mini.rpc('getDisplaySchedule'), { enabled: true, action: 'off', ...around });
  await until(brightness, value => value === 0, 45);
  // A session that ends and starts is a state change; it does not wake the
  // display in the quiet hours.
  await mini.leave();
  await mini.open();
  assert.equal(await brightness(), 0, 'a new session did not wake the display in its quiet hours');

  await store({ enabled: true, action: 'dim', ...around });
  await until(brightness, value => value === BRIGHTNESS / 2, 10);

  // The same times of day in UTC are not now, so the zone decides.
  await store({ enabled: true, action: 'off', ...around, timezone: 'UTC' });
  await until(brightness, value => value === BRIGHTNESS, 10);

  // What the Mini cannot use, it refuses and does not store.
  await assert.rejects(store({ enabled: true, action: 'off', ...around, timezone: 'Europe/Atlantis' }));
  await assert.rejects(store({ enabled: true, action: 'off', ...around, end: around.start }));
  assert.equal((await mini.rpc('getDisplaySchedule')).timezone, 'UTC');

  // A stored schedule is in use after a restart, once the Mini has the time.
  await store({ enabled: true, action: 'off', ...around });
  const startedAt = Date.now();
  mini.rpc('reboot', { force: true }, 5000).catch(() => undefined);
  await until(() => mini.restarted(startedAt), restarted => restarted, 120);
  await mini.open();
  assert.deepEqual(await mini.rpc('getDisplaySchedule'), { enabled: true, action: 'off', ...around });
  await until(brightness, value => value === 0, 60);

  await store({ enabled: false, action: 'off', ...around });
  await until(brightness, value => value === BRIGHTNESS, 10);
});
