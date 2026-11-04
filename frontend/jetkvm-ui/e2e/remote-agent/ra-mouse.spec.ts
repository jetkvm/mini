import type { Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
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
});
