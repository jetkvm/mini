// The display schedule menu and its time fields (src/utils/displaySchedule.ts).
// Run from frontend/jetkvm-ui with Node 22.21.1: node --test tests/display-schedule.mjs
import assert from "node:assert/strict";
import { test } from "node:test";
import {
  isTimeOfDay,
  menuOfSchedule,
  scheduleForMenu,
  settingWriter,
} from "../src/utils/displaySchedule.ts";

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

// A page for settingWriter(): what the controls show, the notifications, and
// the writes on their way, which the test answers one by one.
function page() {
  const sent = [];
  const log = [];
  let shown;
  const writer = settingWriter({
    send: value => new Promise(resolve => sent.push({ value, answer: resolve })),
    show: value => {
      shown = value;
    },
    succeeded: () => log.push("stored"),
    failed: error => log.push(`failed: ${error}`),
  });
  // Lets the writer run after an answer.
  const settle = () => new Promise(resolve => setImmediate(resolve));
  return { writer, sent, log, settle, shown: () => shown };
}

test("an edit shows at once and is stored", async () => {
  const { writer, sent, log, settle, shown } = page();
  writer.read("found");
  assert.equal(shown(), "found");
  writer.write("A");
  assert.equal(shown(), "A");
  assert.deepEqual(
    sent.map(each => each.value),
    ["A"],
  );
  sent[0].answer(null);
  await settle();
  assert.equal(shown(), "A");
  assert.deepEqual(log, ["stored"]);
});

test("a refused edit shows the value that the device has", async () => {
  const { writer, sent, log, settle, shown } = page();
  writer.read("found");
  writer.write("A");
  sent[0].answer("Unknown time zone");
  await settle();
  assert.equal(shown(), "found");
  assert.deepEqual(log, ["failed: Unknown time zone"]);
});

test("an edit during a write waits for its answer, and the newest edit is sent", async () => {
  const { writer, sent, log, settle, shown } = page();
  writer.read("found");
  writer.write("A");
  writer.write("B");
  writer.write("C");
  assert.equal(shown(), "C");
  assert.deepEqual(
    sent.map(each => each.value),
    ["A"],
  );
  sent[0].answer(null);
  await settle();
  assert.deepEqual(
    sent.map(each => each.value),
    ["A", "C"],
  );
  assert.deepEqual(log, []);
  sent[1].answer(null);
  await settle();
  assert.equal(shown(), "C");
  assert.deepEqual(log, ["stored"]);
});

test("after a refused edit, a later edit that is stored stays on the page", async () => {
  const { writer, sent, log, settle, shown } = page();
  writer.read("found");
  writer.write("A");
  writer.write("B");
  sent[0].answer("refused");
  await settle();
  assert.equal(shown(), "B");
  sent[1].answer(null);
  await settle();
  assert.equal(shown(), "B");
  assert.deepEqual(log, ["failed: refused", "stored"]);
});

test("when the later edit fails, the page shows the edit that the device stored", async () => {
  const { writer, sent, log, settle, shown } = page();
  writer.read("found");
  writer.write("A");
  writer.write("B");
  sent[0].answer(null);
  await settle();
  // B is the one that could not be sent, or that the device refused.
  sent[1].answer("not sent");
  await settle();
  assert.equal(shown(), "A");
  assert.deepEqual(log, ["failed: not sent"]);
});

test("the answer to a read during a write is not shown over the edit", async () => {
  const { writer, sent, log, settle, shown } = page();
  writer.read("found");
  writer.write("A");
  writer.read("found");
  assert.equal(shown(), "A");
  sent[0].answer(null);
  await settle();
  assert.equal(shown(), "A");
  assert.deepEqual(log, ["stored"]);
});

test("a failed write shows what the device reported during it", async () => {
  const { writer, sent, settle, shown } = page();
  writer.read("found");
  writer.write("A");
  // Another page changed the setting; this page's read saw it.
  writer.read("other");
  sent[0].answer("refused");
  await settle();
  assert.equal(shown(), "other");
});

test("after a channel change, an answer to an old write changes nothing and edits are sent again", async () => {
  const { writer, sent, log, settle, shown } = page();
  writer.read("found");
  writer.write("A");
  writer.reset();
  writer.read("found");
  assert.equal(shown(), "found");
  writer.write("B");
  assert.deepEqual(
    sent.map(each => each.value),
    ["A", "B"],
  );
  sent[0].answer(null);
  await settle();
  assert.equal(shown(), "B");
  assert.deepEqual(log, []);
  sent[1].answer("refused");
  await settle();
  assert.equal(shown(), "found");
  assert.deepEqual(log, ["failed: refused"]);
});
