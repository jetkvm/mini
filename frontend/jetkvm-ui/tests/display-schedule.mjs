// The display schedule menu and its time fields (src/utils/displaySchedule.ts).
// Run from frontend/jetkvm-ui with Node 22.21.1: node --test tests/display-schedule.mjs
import assert from "node:assert/strict";
import { test } from "node:test";
import { isTimeOfDay, menuOfSchedule, scheduleForMenu } from "../src/utils/displaySchedule.ts";

const ZONES = ["Asia/Tokyo", "Europe/Stockholm", "UTC"];
// What the device answers before a schedule was stored: no time zone.
const UNSET = { enabled: false, action: "off", start: "22:00", end: "06:00", timezone: "" };

test("the menu shows the action of a schedule that is on, and Never otherwise", () => {
  assert.equal(menuOfSchedule(UNSET), "never");
  assert.equal(menuOfSchedule({ ...UNSET, action: "dim" }), "never");
  assert.equal(menuOfSchedule({ ...UNSET, enabled: true }), "off");
  assert.equal(menuOfSchedule({ ...UNSET, enabled: true, action: "dim" }), "dim");
});

test("a schedule turned on for the first time takes the browser's time zone", () => {
  assert.deepEqual(scheduleForMenu(UNSET, "off", ZONES, "Europe/Stockholm"), {
    enabled: true,
    action: "off",
    start: "22:00",
    end: "06:00",
    timezone: "Europe/Stockholm",
  });
  assert.equal(scheduleForMenu(UNSET, "dim", ZONES, "Asia/Tokyo").action, "dim");
});

test("a browser time zone that the device does not have gives a first schedule in UTC", () => {
  assert.equal(scheduleForMenu(UNSET, "off", ZONES, "Asia/Calcutta").timezone, "UTC");
  // The list has not arrived yet.
  assert.equal(scheduleForMenu(UNSET, "off", [], "Europe/Stockholm").timezone, "UTC");
});

test("a stored time zone stays when the schedule is turned on again or its action changes", () => {
  const stored = { ...UNSET, timezone: "Asia/Tokyo" };
  assert.equal(scheduleForMenu(stored, "off", ZONES, "Europe/Stockholm").timezone, "Asia/Tokyo");
  // UTC that the user selected is a stored zone as any other.
  const utc = { ...UNSET, timezone: "UTC" };
  assert.equal(scheduleForMenu(utc, "dim", ZONES, "Europe/Stockholm").timezone, "UTC");
  const on = { ...utc, enabled: true };
  assert.deepEqual(scheduleForMenu(on, "dim", ZONES, "Europe/Stockholm"), { ...on, action: "dim" });
});

test("Never turns the schedule off and keeps the rest", () => {
  const on = { enabled: true, action: "dim", start: "23:30", end: "05:15", timezone: "Asia/Tokyo" };
  assert.deepEqual(scheduleForMenu(on, "never", ZONES, "Europe/Stockholm"), {
    ...on,
    enabled: false,
  });
});

test("a time field is stored only as a whole time of day", () => {
  for (const value of ["00:00", "06:05", "19:59", "23:59"]) assert.equal(isTimeOfDay(value), true);
  for (const value of ["", "6:05", "24:00", "12:60", "12:00:00", "12-00", " 12:00"]) {
    assert.equal(isTimeOfDay(value), false, value);
  }
});
