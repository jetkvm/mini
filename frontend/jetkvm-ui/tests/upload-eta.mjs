// The upload view's rate and remaining time (src/utils/uploadEta.ts).
// Run from frontend/jetkvm-ui with Node 22.21.1: node --test tests/upload-eta.mjs
import assert from "node:assert/strict";
import { test } from "node:test";
import {
  addRateSample,
  remainingSeconds,
  remainingTime,
  uploadRate,
} from "../src/utils/uploadEta.ts";

const MB = 1024 * 1024;

// Readings every 500 ms from `start` to `end` (ms) at `bytesPerSecond`, from `bytes` on.
function feed(samples, start, end, bytesPerSecond, bytes) {
  for (let time = start; time <= end; time += 500) {
    addRateSample(samples, time, bytes + ((time - start) / 1000) * bytesPerSecond);
  }
  return bytes + ((end - start) / 1000) * bytesPerSecond;
}

test("no rate before the readings span 3 s", () => {
  const samples = [];
  feed(samples, 0, 2500, 2 * MB, 0);
  assert.equal(uploadRate(samples, 2500), null);
  assert.equal(uploadRate(samples, 2999), null);
  assert.equal(uploadRate(samples, 3000), (5 * MB) / 3);
});

test("a resumed upload measures only the bytes sent in this attempt", () => {
  const samples = [];
  feed(samples, 0, 4000, 2 * MB, 700 * MB);
  assert.equal(uploadRate(samples, 4000), 2 * MB);
});

test("the rate follows the last 10 s, not the whole upload", () => {
  const samples = [];
  const bytes = feed(samples, 0, 60_000, 4 * MB, 0);
  feed(samples, 60_500, 80_000, 1 * MB, bytes + 0.5 * MB);
  // 60 s at 4 MB/s, then 20 s at 1 MB/s: the whole-upload mean is 3.25 MB/s.
  assert.equal(uploadRate(samples, 80_000), 1 * MB);
  // The window keeps one reading at or before its start, nothing older.
  assert.ok(samples.length <= 22, `kept ${samples.length} readings`);
  assert.ok(samples[1].time > 70_000);
});

test("a stall lowers the rate as time passes and gives 0 after a full window", () => {
  const samples = [];
  feed(samples, 0, 20_000, 2 * MB, 0);
  assert.equal(uploadRate(samples, 20_000), 2 * MB);
  // No reading arrives after 20 s.
  assert.equal(uploadRate(samples, 25_000), (2 * MB * 5) / 10);
  assert.equal(uploadRate(samples, 30_000), 0);
  assert.equal(uploadRate(samples, 600_000), 0);
});

test("remaining time is hidden for no rate, a stall, a sent file and more than a day", () => {
  assert.equal(remainingSeconds(null, 100 * MB), null);
  assert.equal(remainingSeconds(0, 100 * MB), null);
  // 4 GB at 40 KB/s is about 29 hours.
  assert.equal(remainingSeconds(40 * 1024, 4096 * MB), null);
  assert.equal(remainingSeconds(2 * MB, 100 * MB), 50);
  assert.equal(remainingSeconds(2 * MB, 0), null);
});

test("remaining time rounds to 5 s, then minutes, then hours and minutes", () => {
  assert.deepEqual(remainingTime(0), { unit: "s", seconds: 5 });
  assert.deepEqual(remainingTime(41), { unit: "s", seconds: 45 });
  assert.deepEqual(remainingTime(55), { unit: "s", seconds: 55 });
  assert.deepEqual(remainingTime(56), { unit: "min", minutes: 1 });
  assert.deepEqual(remainingTime(150), { unit: "min", minutes: 3 });
  assert.deepEqual(remainingTime(3569), { unit: "min", minutes: 59 });
  assert.deepEqual(remainingTime(3570), { unit: "h", hours: 1, minutes: 0 });
  assert.deepEqual(remainingTime(5 * 3600 + 20 * 60), { unit: "h", hours: 5, minutes: 20 });
});
