import type { Page } from "@playwright/test";
import { test, expect } from "./helpers/test";
import {
  ensureNoPasswordViaAPI,
  waitForDecodedFrames,
  waitForVideoStream,
  waitForWebRTCReady,
} from "./helpers";

const SIGNALING = /webrtc\/signaling/;
const WAIT_LIMIT_MS = 10_000;
const OFFERS_WITH_RETRIES = 4;

const card = (page: Page) => page.getByTestId("connection-card");

const cardState = (page: Page) =>
  card(page)
    .first()
    .evaluate(el => `${el.dataset.status}:${el.dataset.question}`)
    .catch(() => null);

async function stubSignaling(page: Page, onOffer?: (send: (data: string) => void) => void) {
  const offers = { count: 0 };
  await page.routeWebSocket(SIGNALING, ws => {
    ws.send(JSON.stringify({ type: "device-metadata", data: { deviceVersion: "e2e" } }));
    ws.onMessage(message => {
      if (typeof message !== "string" || !message.startsWith("{")) return;
      if (JSON.parse(message).type !== "offer") return;
      offers.count++;
      onOffer?.(data => ws.send(data));
    });
  });
  return offers;
}

test.describe("Connection card", () => {
  test.beforeAll(async () => {
    await ensureNoPasswordViaAPI();
  });

  test("retries a missing answer, then says the KVM did not answer", async ({ page }) => {
    test.setTimeout(120_000);
    const offers = await stubSignaling(page);
    await page.goto("/");

    await expect.poll(() => cardState(page)).toBe("waiting:4");
    await expect.poll(() => offers.count, { timeout: 90_000 }).toBe(OFFERS_WITH_RETRIES);
    const lastOffer = Date.now();
    await expect.poll(() => cardState(page), { timeout: WAIT_LIMIT_MS + 10_000 }).toBe("failed:4");
    expect(Date.now() - lastOffer).toBeGreaterThan(WAIT_LIMIT_MS - 2_000);
    expect(offers.count).toBe(OFFERS_WITH_RETRIES);
  });

  test("retries a rejected answer, then fails without the wait", async ({ page }) => {
    test.setTimeout(90_000);
    const invalidAnswer = Buffer.from(
      JSON.stringify({ type: "answer", sdp: "v=0\r\nnot a session description\r\n" }),
    ).toString("base64");
    const offers = await stubSignaling(page, send =>
      send(JSON.stringify({ type: "answer", data: invalidAnswer })),
    );
    await page.goto("/");

    await expect.poll(() => offers.count, { timeout: 60_000 }).toBe(OFFERS_WITH_RETRIES);
    const lastOffer = Date.now();
    await expect.poll(() => cardState(page), { timeout: 5_000 }).toBe("failed:4");
    expect(Date.now() - lastOffer).toBeLessThan(WAIT_LIMIT_MS - 2_000);
  });

  test("fails at once when this computer goes offline", async ({ page, context }) => {
    await page.routeWebSocket(SIGNALING, ws => ws.close());
    await page.goto("/");
    await expect.poll(() => cardState(page)).toBe("waiting:3");

    await context.setOffline(true);
    try {
      await expect.poll(() => cardState(page), { timeout: 5_000 }).toBe("failed:3");
    } finally {
      await context.setOffline(false);
    }
  });

  test("shows the loading state, then hides it when the video plays", async ({ page }) => {
    test.setTimeout(90_000);
    await page.goto("/");

    await expect.poll(() => cardState(page)).toMatch(/^waiting:/);
    await waitForWebRTCReady(page);
    await waitForVideoStream(page);
    await waitForDecodedFrames(page);

    await expect(card(page)).toHaveCount(0, { timeout: 15_000 });
    const video = page.locator("video");
    await expect.poll(() => video.evaluate(el => getComputedStyle(el).opacity)).toBe("1");
    expect(await video.evaluate(el => !(el as HTMLVideoElement).paused)).toBe(true);
  });
});
