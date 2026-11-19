// The connection diagnosis (src/diagnosis/diagnosis.ts).
// Run from frontend/jetkvm-ui with Node 22.21.1: node --test tests/diagnosis.mjs
import assert from "node:assert/strict";
import { test } from "node:test";
import {
  FINDING_IDS,
  SCENARIOS,
  applyChange,
  diagnose,
  healthyInput,
  leadFinding,
} from "../src/diagnosis/diagnosis.ts";

const run = set => diagnose(applyChange(healthyInput(), set));
const ids = diagnosis =>
  [diagnosis.headline, ...diagnosis.others].filter(Boolean).map(finding => finding.id);
const status = (diagnosis, check) => diagnosis.checks.find(c => c.id === check).status;

for (const scenario of SCENARIOS) {
  test(`scenario: ${scenario.name}`, () => {
    const diagnosis = run(scenario.set);
    assert.equal(diagnosis.headline?.id ?? null, scenario.headline);
    for (const id of scenario.also ?? []) {
      assert.ok(ids(diagnosis).includes(id), `${id} is missing from ${ids(diagnosis).join(", ")}`);
    }
  });
}

test("the healthy state has no finding and every check passes", () => {
  const diagnosis = run({});
  assert.deepEqual(ids(diagnosis), []);
  assert.deepEqual(
    diagnosis.checks.map(check => check.status),
    Array(8).fill("pass"),
  );
});

test("each finding is reached by a scenario", () => {
  const reached = new Set(SCENARIOS.flatMap(scenario => ids(run(scenario.set))));
  assert.deepEqual(
    FINDING_IDS.filter(id => !reached.has(id)),
    [],
  );
});

test("a failed connection does not report the state of the computer", () => {
  // Without a connection the JetKVM cannot report HDMI or USB, so a sleeping
  // computer must not show as a finding or change the result.
  const blocked = run({ ice: "failed" });
  const blockedAndAsleep = run({ ice: "failed", hdmi: "no_signal", usb: "suspended" });
  assert.deepEqual(blockedAndAsleep, blocked);
  for (const check of ["health", "picture", "input", "audio", "quality"]) {
    assert.equal(status(blocked, check), "skipped");
  }
});

test("a browser fault stops before the JetKVM is contacted", () => {
  const diagnosis = run({ h264: "missing", http: "unreachable" });
  assert.equal(diagnosis.headline.id, "no_h264");
  assert.equal(status(diagnosis, "reach"), "skipped");
});

test("a sleeping computer is one problem, not a picture fault and a USB fault", () => {
  const diagnosis = run({ hdmi: "no_signal", usb: "suspended" });
  const problems = [diagnosis.headline, ...diagnosis.others].filter(f => f.level === "problem");
  assert.deepEqual(
    problems.map(f => f.id),
    ["computer_asleep"],
  );
});

test("picture, input and audio faults show together when the connection is up", () => {
  const diagnosis = run({ hdmi: "unsupported_mode", keyboardTest: "no_reply" });
  assert.equal(diagnosis.headline.id, "unsupported_mode");
  assert.ok(ids(diagnosis).includes("keyboard_no_reply"));
});

test("audio is not checked without a configured USB device", () => {
  assert.equal(status(run({ usb: "not_attached", audioSource: "not_ready" }), "audio"), "skipped");
  // With audio off there is nothing to check, so the USB state does not matter.
  assert.equal(status(run({ usb: "not_attached", audioSetting: "off" }), "audio"), "info");
});

test("quality is not judged without a picture", () => {
  const diagnosis = run({ hdmi: "no_cable", loss: "high" });
  assert.equal(status(diagnosis, "quality"), "skipped");
  assert.ok(!ids(diagnosis).includes("packet_loss"));
});

test("the default EDID is offered only when the EDID is not the default", () => {
  assert.equal(run({ hdmi: "unsupported_mode" }).headline.action, undefined);
  assert.equal(
    run({ hdmi: "unsupported_mode", edid: "custom" }).headline.action,
    "Use the default EDID",
  );
});

test("applying a finding's fix removes that finding", () => {
  for (const scenario of SCENARIOS) {
    const input = applyChange(healthyInput(), scenario.set);
    const diagnosis = diagnose(input);
    for (const finding of [diagnosis.headline, ...diagnosis.others]) {
      if (!finding?.resolves) continue;
      const after = diagnose(applyChange(input, finding.resolves));
      assert.ok(
        !ids(after).includes(finding.id),
        `${scenario.id}: ${finding.id} stays after its fix`,
      );
    }
  }
});

test("the map marks what is broken and what could not be checked", () => {
  // A network fault: nothing behind the network can be known.
  assert.deepEqual(run({ ice: "failed" }).map, {
    browser: "ok",
    network: "problem",
    mini: "unknown",
    hdmi: "unknown",
    usb: "unknown",
    computer: "unknown",
  });
  // A sleeping computer: the cables are not at fault, so they are not red.
  assert.deepEqual(run({ hdmi: "no_signal", usb: "suspended" }).map, {
    browser: "ok",
    network: "ok",
    mini: "ok",
    hdmi: "unknown",
    usb: "unknown",
    computer: "problem",
  });
  // Two faults behind a working connection both show.
  const two = run({ hdmi: "unsupported_mode", usb: "attached" }).map;
  assert.equal(two.hdmi, "problem");
  assert.equal(two.usb, "problem");
  assert.equal(two.computer, "ok");
});

test("a browser fault leaves the rest of the map unknown", () => {
  const map = run({ h264: "missing" }).map;
  assert.equal(map.browser, "problem");
  for (const place of ["network", "mini", "hdmi", "usb", "computer"]) {
    assert.equal(map[place], "unknown");
  }
});

test("without a problem, the panel leads with a warning before a note", () => {
  const diagnosis = run({ restart: "power", link: "wifi_weak" });
  assert.equal(diagnosis.headline, null);
  assert.equal(leadFinding(diagnosis).id, "wifi_weak");
  assert.equal(leadFinding(run({})), null);
});
