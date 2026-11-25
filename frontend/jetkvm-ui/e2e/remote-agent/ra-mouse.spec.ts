import type { Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
import { sendWheel } from "../helpers";
import { type MouseEvent as RAMouseEvent } from "./remote-agent";
import { agent, registerSharedSession } from "./shared";

test.describe.configure({ mode: "serial" });

let sharedPage: Page;
registerSharedSession(page => (sharedPage = page));

test.describe("Remote Host Agent: mouse", () => {
  // ═══════════════════════════════════════════
  // MOUSE: BLUR DOES NOT JUMP TO TOP-LEFT
  // ═══════════════════════════════════════════

  test("mouse: window blur releases buttons without moving cursor", async () => {
    // Move mouse to center of the video element via a real mousemove event
    // so that useMouse's lastAbsPos is updated through the normal code path.
    const video = sharedPage.locator("video");
    const box = await video.boundingBox();
    expect(box).not.toBeNull();

    const centerX = box!.x + box!.width / 2;
    const centerY = box!.y + box!.height / 2;

    // Move to center — this triggers the real absMouseMoveHandler
    await sharedPage.mouse.move(centerX, centerY);
    await new Promise(r => setTimeout(r, 100));

    // Clear events, then dispatch blur
    await agent!.clearMouseEvents();
    await sharedPage.evaluate(() => window.dispatchEvent(new Event("blur")));
    await new Promise(r => setTimeout(r, 200));

    // Collect any mouse events that were sent on blur
    const events = await agent!.getMouseEvents();
    const absEvents = events.filter(ev => ev.type === "mouse_move_abs");

    // If any abs mouse events were sent, none should be at (0, 0)
    for (const ev of absEvents) {
      expect(
        ev.x > 100 || ev.y > 100,
        `Blur should not move cursor to origin, got (${ev.x}, ${ev.y})`,
      ).toBe(true);
    }
  });

  // ═══════════════════════════════════════════
  // MOUSE: WHEEL SCROLL (VERTICAL + HORIZONTAL)
  // ═══════════════════════════════════════════

  test("mouse: vertical and horizontal wheel scroll", async () => {
    const REL_WHEEL = 0x08;
    const REL_HWHEEL = 0x06;

    // Vertical scroll
    await agent!.clearMouseEvents();
    await sendWheel(sharedPage, 1, 0);
    const vWheel = await agent!.waitForMouseEvent(
      ev => ev.type === "mouse_move_rel" && ev.code === REL_WHEEL,
      3000,
    );
    expect(vWheel.length, "Vertical wheel event should be received").toBeGreaterThan(0);
    expect(vWheel[0].value, "Positive wheelY scrolls up").toBe(1);

    // Horizontal scroll
    await agent!.clearMouseEvents();
    await sendWheel(sharedPage, 0, 1);
    const hWheel = await agent!.waitForMouseEvent(
      ev => ev.type === "mouse_move_rel" && ev.code === REL_HWHEEL,
      3000,
    );
    expect(hWheel.length, "Horizontal wheel event should be received").toBeGreaterThan(0);
    expect(hWheel[0].value, "Positive wheelX scrolls right").toBe(1);

    // Both axes simultaneously
    await agent!.clearMouseEvents();
    await sendWheel(sharedPage, -1, 1);
    const bothV = await agent!.waitForMouseEvent(
      ev => ev.type === "mouse_move_rel" && ev.code === REL_WHEEL,
      3000,
    );
    expect(bothV.length, "Vertical wheel in combined event").toBeGreaterThan(0);
    expect(bothV[0].value, "Negative wheelY scrolls down").toBe(-1);
    const bothH = await agent!.waitForMouseEvent(
      ev => ev.type === "mouse_move_rel" && ev.code === REL_HWHEEL,
      3000,
    );
    expect(bothH.length, "Horizontal wheel in combined event").toBeGreaterThan(0);
    expect(bothH[0].value, "Positive wheelX scrolls right").toBe(1);
  });

  test("mouse: wheel scroll should not produce duplicate events from both HID devices", async () => {
    test.setTimeout(30_000);
    const REL_WHEEL = 0x08;
    const REL_HWHEEL = 0x06;

    await agent!.waitForInputDevices(["keyboard", "absolute_mouse", "relative_mouse"], 10000);

    // Retry until the agent starts receiving wheel events (host may still
    // be re-enumerating HID devices after previous test's USB changes).
    const vDeadline = Date.now() + 10000;
    let vWheelEvents: RAMouseEvent[] = [];
    while (Date.now() < vDeadline) {
      await agent!.clearMouseEvents();
      await sendWheel(sharedPage, 1, 0);
      try {
        await agent!.waitForMouseEvent(
          ev => ev.type === "mouse_move_rel" && ev.code === REL_WHEEL,
          2000,
        );
        break;
      } catch {
        /* agent not ready yet, retry */
      }
    }
    // Allow any duplicate from the second HID device to arrive
    await new Promise(r => setTimeout(r, 500));

    const allEvents = await agent!.getMouseEvents();
    vWheelEvents = allEvents.filter(ev => ev.type === "mouse_move_rel" && ev.code === REL_WHEEL);
    const devices = new Set(vWheelEvents.map(ev => ev.device));
    console.log(
      `Vertical wheel: ${vWheelEvents.length} event(s) from device(s): ${[...devices].join(", ")}`,
    );
    expect(
      vWheelEvents.length,
      `Expected 1 vertical wheel event, got ${vWheelEvents.length} from [${[...devices].join(", ")}]. ` +
        `Duplicate means rpcWheelReport sends to both abs and rel mouse HID devices.`,
    ).toBe(1);

    await agent!.clearMouseEvents();
    await sendWheel(sharedPage, 0, 1);
    await agent!.waitForMouseEvent(
      ev => ev.type === "mouse_move_rel" && ev.code === REL_HWHEEL,
      3000,
    );
    await new Promise(r => setTimeout(r, 500));

    const allHEvents = await agent!.getMouseEvents();
    const hWheelEvents = allHEvents.filter(
      ev => ev.type === "mouse_move_rel" && ev.code === REL_HWHEEL,
    );
    const hDevices = new Set(hWheelEvents.map(ev => ev.device));
    console.log(
      `Horizontal wheel: ${hWheelEvents.length} event(s) from device(s): ${[...hDevices].join(", ")}`,
    );
    expect(
      hWheelEvents.length,
      `Expected 1 horizontal wheel event, got ${hWheelEvents.length} from [${[...hDevices].join(", ")}].`,
    ).toBe(1);
  });
});
