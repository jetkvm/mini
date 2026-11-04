import type { Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
import {
  HID_KEY,
  callJsonRpc,
  getKeysDownState,
  sendKeypress,
  tapKey,
  waitForWebRTCReady,
  ensureRpcReady,
  getLedState,
  waitForLedState,
} from "../helpers";
import { waitForKeyboardReady, KEY, type KeyboardEvent as RAKeyboardEvent } from "./remote-agent";
import { ALL_SCAN_KEYS, agent, registerSharedSession } from "./shared";
import { captureKeyboard } from "./keyboard-capture";

test.describe.configure({ mode: "serial" });

let sharedPage: Page;
registerSharedSession(page => (sharedPage = page));

let finishCapture: Awaited<ReturnType<typeof captureKeyboard>> | undefined;
test.beforeEach(async () => {
  finishCapture = undefined;
  if (agent) finishCapture = await captureKeyboard(sharedPage, agent);
});
// oxlint-disable-next-line no-empty-pattern
test.afterEach(async ({}, testInfo) => {
  await finishCapture?.(testInfo);
});

test.describe("Remote Host Agent: keyboard", () => {
  // ═══════════════════════════════════════════
  // KEYBOARD: TOGGLE KEYS + LED ROUND-TRIP
  // ═══════════════════════════════════════════

  test("keyboard: toggle keys with LED round-trip", async () => {
    test.setTimeout(30_000);

    const initialState = await getLedState(sharedPage);
    expect(initialState).not.toBeNull();

    // EDID restore may still be in-flight; retry until full HID stack (including LED reports) stabilizes.
    // Re-read caps_lock each iteration so a failed-try + successful-undo doesn't leave us
    // permanently toggling in the wrong direction.
    const deadline = Date.now() + 15000;
    let capsToggled = false;
    let capsBeforeToggle = initialState!.caps_lock;
    while (Date.now() < deadline) {
      capsBeforeToggle = (await getLedState(sharedPage))!.caps_lock;
      await agent!.clearKeyboardEvents();
      try {
        await agent!.expectKeyPress(
          KEY.CAPS_LOCK,
          async () => {
            await tapKey(sharedPage, HID_KEY.CAPS_LOCK);
          },
          3000,
        );
        await waitForLedState(sharedPage, "caps_lock", !capsBeforeToggle, 2000);
        capsToggled = true;
        break;
      } catch {
        await tapKey(sharedPage, HID_KEY.CAPS_LOCK);
        await new Promise(r => setTimeout(r, 500));
      }
    }
    expect(capsToggled, "CAPS_LOCK LED should toggle").toBe(true);
    expect((await getLedState(sharedPage))!.caps_lock).toBe(!capsBeforeToggle);

    // Restore CAPS_LOCK
    await agent!.expectKeyPress(
      KEY.CAPS_LOCK,
      async () => {
        await tapKey(sharedPage, HID_KEY.CAPS_LOCK);
      },
      5000,
    );
    await waitForLedState(sharedPage, "caps_lock", capsBeforeToggle);

    // NUM_LOCK: same round-trip verification
    const initialNum = initialState!.num_lock;

    const numEvents = await agent!.expectKeyPress(
      KEY.NUM_LOCK,
      async () => {
        await tapKey(sharedPage, HID_KEY.NUM_LOCK);
      },
      5000,
    );
    expect(numEvents.length).toBeGreaterThan(0);
    await waitForLedState(sharedPage, "num_lock", !initialNum);
    expect((await getLedState(sharedPage))!.num_lock).toBe(!initialNum);

    await agent!.expectKeyPress(
      KEY.NUM_LOCK,
      async () => {
        await tapKey(sharedPage, HID_KEY.NUM_LOCK);
      },
      5000,
    );
    await waitForLedState(sharedPage, "num_lock", initialNum);

    // SPACE: verify received (no LED, just key delivery)
    const spaceEvents = await agent!.expectKeyPress(
      KEY.SPACE,
      async () => {
        await tapKey(sharedPage, HID_KEY.SPACE);
      },
      5000,
    );
    expect(spaceEvents.length).toBeGreaterThan(0);
  });

  // ═══════════════════════════════════════════
  // KEYBOARD: SCANS + PRESS/RELEASE + MODIFIERS
  // ═══════════════════════════════════════════

  test("keyboard: key scans, press/release, and modifiers", async () => {
    // Batch all 48 key scans in a single evaluate (eliminates per-key round-trip overhead)
    await agent!.clearKeyboardEvents();

    await sharedPage.evaluate(
      async (keys: number[]) => {
        const hooks = window.__kvmTestHooks;
        if (!hooks) throw new Error("Test hooks not available");
        for (const hid of keys) {
          hooks.sendKeypress(hid, true);
          hooks.sendKeypress(hid, false);
          await new Promise(r => setTimeout(r, 10));
        }
      },
      ALL_SCAN_KEYS.map(k => k.hid),
    );

    const scanDeadline = Date.now() + 5000;
    let failed: string[] = [];
    while (Date.now() < scanDeadline) {
      const events = await agent!.getKeyboardEvents();
      const pressedCodes = new Set(events.filter(ev => ev.type === "key_press").map(ev => ev.code));
      failed = ALL_SCAN_KEYS.filter(k => !pressedCodes.has(k.linux)).map(k => k.label);
      if (failed.length === 0) break;
      await new Promise(r => setTimeout(r, 50));
    }
    expect(failed, `Keys not received: ${failed.join(", ")}`).toHaveLength(0);

    // Press/release timing: verify release comes after press
    await agent!.clearKeyboardEvents();
    await sendKeypress(sharedPage, HID_KEY.SPACE, true);
    await new Promise(r => setTimeout(r, 10));
    await sendKeypress(sharedPage, HID_KEY.SPACE, false);
    await expect(async () => {
      const prEvents = await agent!.getKeyboardEvents();
      const presses = prEvents.filter(ev => ev.code === KEY.SPACE && ev.type === "key_press");
      const releases = prEvents.filter(ev => ev.code === KEY.SPACE && ev.type === "key_release");
      expect(presses.length).toBeGreaterThanOrEqual(1);
      expect(releases.length).toBeGreaterThanOrEqual(1);
      expect(releases[0].time_ms).toBeGreaterThan(presses[0].time_ms);
    }).toPass({ timeout: 5000, intervals: [50] });

    // Modifier combo: verify C key arrives
    await agent!.clearKeyboardEvents();
    await sendKeypress(sharedPage, 0x06, true);
    await new Promise(r => setTimeout(r, 10));
    await sendKeypress(sharedPage, 0x06, false);
    await expect(async () => {
      const cEvents = await agent!.getKeyboardEvents();
      const cPresses = cEvents.filter(ev => ev.code === KEY.C && ev.type === "key_press");
      expect(cPresses.length).toBeGreaterThanOrEqual(1);
    }).toPass({ timeout: 5000, intervals: [50] });
  });

  // ═══════════════════════════════════════════
  // KEYBOARD: KEEPALIVE & AUTO-RELEASE
  // ═══════════════════════════════════════════

  test("keepalive: key auto-releases after window blur, no stuck keys on re-focus", async () => {
    // When the browser loses focus, resetKeyboardState fires: cancels keepalives
    // and sends a zero-key report. The device then auto-releases any held keys.
    // On re-focus, no phantom presses should appear.
    await agent!.clearKeyboardEvents();

    await sendKeypress(sharedPage, 0x04, true);
    await new Promise(r => setTimeout(r, 100));

    // Blur triggers resetKeyboardState — cancels keepalives, sends zero-key report
    await sharedPage.evaluate(() => window.dispatchEvent(new Event("blur")));

    // Wait for device-side auto-release (100ms timer + network margin)
    await new Promise(r => setTimeout(r, 400));

    const events = await agent!.getKeyboardEvents();
    const releases = events.filter(ev => ev.code === KEY.A && ev.type === "key_release");
    expect(releases.length, "Key A should auto-release after blur").toBeGreaterThanOrEqual(1);

    // Re-focus and verify no phantom key events
    await agent!.clearKeyboardEvents();
    await sharedPage.evaluate(() => window.dispatchEvent(new Event("focus")));
    await new Promise(r => setTimeout(r, 200));

    const focusEvents = await agent!.getKeyboardEvents();
    const stuckPresses = focusEvents.filter(ev => ev.type === "key_press");
    expect(stuckPresses.length, "No stuck keys after re-focus").toBe(0);
  });

  test("keepalive: window blur clears lone modifier", async () => {
    await agent!.clearKeyboardEvents();

    await sendKeypress(sharedPage, 0xe1, true);
    await expect
      .poll(
        async () => {
          const state = await getKeysDownState(sharedPage);
          return state?.modifier === 0x02;
        },
        {
          message: "LeftShift should be held before blur",
          timeout: 5000,
          intervals: [100, 200, 500],
        },
      )
      .toBe(true);

    await sharedPage.evaluate(() => {
      const target = globalThis as typeof globalThis & {
        dispatchEvent: (event: Event) => boolean;
      };
      target.dispatchEvent(new Event("blur"));
    });

    await expect
      .poll(
        async () => {
          const state = await getKeysDownState(sharedPage);
          const events = await agent!.getKeyboardEvents();
          return (
            state?.modifier === 0 &&
            state.keys.every((k: number) => k === 0) &&
            events.some(ev => ev.code === KEY.LEFT_SHIFT && ev.type === "key_release")
          );
        },
        {
          message: "Window blur should clear LeftShift",
          timeout: 5000,
          intervals: [100, 200, 500],
        },
      )
      .toBe(true);
  });

  test("keepalive: long hold (2s) stays held with keepalives", async () => {
    // Real-world scenario: holding Backspace to delete a line, or holding an arrow
    // key to scroll through code. The key must stay held for the full 2s.
    // Keepalives arrive every 50ms, each extending the 100ms auto-release timer.
    // The key goes down and up as keyboard events of the page, so the
    // keepalives are those of the page's own timer (useKeyboard).
    await agent!.clearKeyboardEvents();

    await sharedPage.keyboard.down("Backspace");
    await new Promise(r => setTimeout(r, 2000));
    await sharedPage.keyboard.up("Backspace");
    await new Promise(r => setTimeout(r, 150));

    const events = await agent!.getKeyboardEvents();
    const presses = events.filter(ev => ev.code === KEY.BACKSPACE && ev.type === "key_press");
    const releases = events.filter(ev => ev.code === KEY.BACKSPACE && ev.type === "key_release");

    expect(presses.length, "Backspace should have exactly one press").toBe(1);
    expect(releases.length, "Backspace should have exactly one release").toBe(1);
    expect(
      events.filter(ev => ev.code !== KEY.BACKSPACE),
      "No other key should have events",
    ).toEqual([]);

    const holdDuration = releases[0].time_ms - presses[0].time_ms;
    console.log(`[keepalive] Backspace held on the host for ${holdDuration}ms`);
    expect(holdDuration, "Backspace should be held for at least 1800ms").toBeGreaterThanOrEqual(
      1800,
    );
  });

  // ═══════════════════════════════════════════
  // KEYBOARD: KEYS RELEASED ON DISCONNECT
  // ═══════════════════════════════════════════

  test("keyboard: all keys released when WebRTC session disconnects", async ({ browser }) => {
    test.setTimeout(30_000);

    const freshPage = await browser.newPage();
    await finishCapture?.watchPage(freshPage);
    await freshPage.goto("/", { waitUntil: "networkidle" });
    await waitForWebRTCReady(freshPage);

    const readyEvents = await waitForKeyboardReady(agent!, freshPage, 15000);
    expect(readyEvents.length, "keyboard should work before disconnect test").toBeGreaterThan(0);

    await agent!.clearKeyboardEvents();

    await sendKeypress(freshPage, 0xe1, true);
    await new Promise(r => setTimeout(r, 20));
    await sendKeypress(freshPage, HID_KEY.SPACE, true);

    await expect
      .poll(
        async () => {
          const state = await getKeysDownState(freshPage);
          return state?.modifier === 0x02 && state.keys.includes(HID_KEY.SPACE);
        },
        {
          message: "LeftShift and Space should be held before disconnect",
          timeout: 5000,
          intervals: [100, 200, 500],
        },
      )
      .toBe(true);

    await expect
      .poll(
        async () => {
          const events = await agent!.getKeyboardEvents();
          return (
            events.some(ev => ev.code === KEY.LEFT_SHIFT && ev.type === "key_press") &&
            events.some(ev => ev.code === KEY.SPACE && ev.type === "key_press")
          );
        },
        {
          message: "Host should see LeftShift and Space presses",
          timeout: 5000,
          intervals: [100, 200, 500],
        },
      )
      .toBe(true);

    // Close the peer directly so browser blur/page-unload cleanup cannot satisfy the test.
    await freshPage.evaluate(() => {
      const peerConnection = (
        globalThis as typeof globalThis & {
          __kvmTestHooks?: { _getPeerConnection?: () => { close: () => void } | null };
        }
      ).__kvmTestHooks?._getPeerConnection?.();
      if (!peerConnection) throw new Error("Peer connection not available");
      peerConnection.close();
    });

    await expect
      .poll(
        async () => {
          const events = await agent!.getKeyboardEvents();
          return (
            events.some(ev => ev.code === KEY.LEFT_SHIFT && ev.type === "key_release") &&
            events.some(ev => ev.code === KEY.SPACE && ev.type === "key_release")
          );
        },
        {
          message: "Host should see LeftShift and Space releases after disconnect",
          timeout: 5000,
          intervals: [100, 200, 500],
        },
      )
      .toBe(true);

    await finishCapture?.stopPage(freshPage);
    await freshPage.close();

    await sharedPage.goto("/", { waitUntil: "networkidle" });
    await ensureRpcReady(sharedPage);

    await expect
      .poll(
        async () => {
          const s = (await callJsonRpc(sharedPage, "getKeyDownState")) as {
            modifier: number;
            keys: number[];
          };
          return s.modifier === 0 && s.keys.every((k: number) => k === 0);
        },
        {
          message: "All key slots should be clear after disconnect",
          timeout: 5000,
          intervals: [200, 500],
        },
      )
      .toBe(true);
  });

  test("keyboard: held modifier released when WebRTC session is replaced", async ({ browser }) => {
    test.setTimeout(30_000);

    const oldPage = await browser.newPage();
    await finishCapture?.watchPage(oldPage);
    let replacementPage: Page | null = null;

    try {
      await oldPage.goto("/", { waitUntil: "networkidle" });
      await ensureRpcReady(oldPage);

      await agent!.clearKeyboardEvents();
      await callJsonRpc(oldPage, "keypressReport", { key: 0xe1, press: true });

      await expect
        .poll(
          async () => {
            const s = (await callJsonRpc(oldPage, "getKeyDownState")) as {
              modifier: number;
              keys: number[];
            };
            return s.modifier === 0x02 && s.keys.every((k: number) => k === 0);
          },
          {
            message: "Old session should hold LeftShift before replacement",
            timeout: 5000,
            intervals: [100, 200, 500],
          },
        )
        .toBe(true);

      replacementPage = await browser.newPage();
      await finishCapture?.watchPage(replacementPage);
      await replacementPage.goto("/", { waitUntil: "networkidle" });
      await ensureRpcReady(replacementPage);

      await expect
        .poll(
          async () => {
            const events = await agent!.getKeyboardEvents();
            return events.some(ev => ev.code === KEY.LEFT_SHIFT && ev.type === "key_release");
          },
          {
            message: "Replacing the session should release LeftShift",
            timeout: 5000,
            intervals: [200, 500],
          },
        )
        .toBe(true);

      await expect
        .poll(
          async () => {
            const s = (await callJsonRpc(replacementPage!, "getKeyDownState")) as {
              modifier: number;
              keys: number[];
            };
            return s.modifier === 0 && s.keys.every((k: number) => k === 0);
          },
          {
            message: "Keyboard state should be clear after session replacement",
            timeout: 5000,
            intervals: [200, 500],
          },
        )
        .toBe(true);
    } finally {
      if (replacementPage) {
        await callJsonRpc(replacementPage, "keypressReport", { key: 0xe1, press: false }).catch(
          () => {},
        );
        await finishCapture?.stopPage(replacementPage);
        await replacementPage.close().catch(() => {});
      }
      await finishCapture?.stopPage(oldPage);
      await oldPage.close().catch(() => {});

      await sharedPage.goto("/", { waitUntil: "networkidle" });
      await ensureRpcReady(sharedPage);
    }
  });

  // ═══════════════════════════════════════════
  // INPUT: MACROS
  // ═══════════════════════════════════════════

  test("input: keyboard macros", async () => {
    test.setTimeout(30_000);

    // Single key press (A) — retry in case the remote agent is still
    // re-opening input devices after the previous USB mode switch.
    const keyABtn = sharedPage.getByRole("button", { name: "E2E KeyA" });
    await keyABtn.waitFor({ state: "visible", timeout: 5000 });

    const macroDeadline = Date.now() + 15000;
    let macroEvents: RAKeyboardEvent[] = [];
    while (Date.now() < macroDeadline) {
      await agent!.clearKeyboardEvents();
      await keyABtn.click();
      try {
        macroEvents = await agent!.waitForKeyboardEvent(
          ev => ev.code === KEY.A && ev.type === "key_press",
          3000,
        );
        break;
      } catch {
        /* agent not ready, retry */
      }
    }
    expect(macroEvents.length).toBeGreaterThan(0);

    // Modifier combo (Ctrl+A)
    await agent!.clearKeyboardEvents();
    await sharedPage.getByRole("button", { name: "E2E Ctrl+A" }).click();

    const ctrlDeadline = Date.now() + 3000;
    let gotCtrl = false,
      gotA = false;
    while (Date.now() < ctrlDeadline && (!gotCtrl || !gotA)) {
      macroEvents = (await agent!.getKeyboardEvents()).filter(ev => ev.type === "key_press");
      for (const ev of macroEvents) {
        if (ev.code === KEY.LEFT_CTRL) gotCtrl = true;
        if (ev.code === KEY.A) gotA = true;
      }
      if (!gotCtrl || !gotA) await new Promise(r => setTimeout(r, 50));
    }
    expect(gotCtrl, "Ctrl key should arrive").toBe(true);
    expect(gotA, "A key should arrive").toBe(true);

    // Key sequence (A, B, C)
    await agent!.clearKeyboardEvents();
    await sharedPage.getByRole("button", { name: "E2E ABC" }).click();

    const expectedSeq = [KEY.A, KEY.B, KEY.C];
    const seqDeadline = Date.now() + 3000;
    let matched = false;
    while (Date.now() < seqDeadline && !matched) {
      const seqEvents = await agent!.getKeyboardEvents();
      const presses = seqEvents.filter(ev => ev.type === "key_press").map(ev => ev.code);
      let idx = 0;
      for (const code of presses) {
        if (code === expectedSeq[idx]) {
          idx++;
          if (idx === expectedSeq.length) {
            matched = true;
            break;
          }
        }
      }
      if (!matched) await new Promise(r => setTimeout(r, 50));
    }
    expect(matched, "Keys A, B, C should arrive in order").toBe(true);
  });
});
