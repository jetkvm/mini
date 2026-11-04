import { test, expect } from "./helpers/test";

import { ensureNoPasswordViaAPI, ensureRpcReady, waitForWebRTCReady } from "./helpers";

// A second page takes the one WebRTC session over. The page that lost the
// session shows the other-session screen with its "Use Here" button and
// its connection goes down, in both directions. That the tabs then keep their state, without a new
// signaling socket or peer connection, and how long each step takes, is
// judged by the walk of tools/combined_load (its takeover state).

// The Mini closes the old peer 1 s after the takeover, and the browser can
// take several more seconds to see it; after this the old page must be down.
const OLD_PEER_MS = 15_000;

test.describe("session takeover", () => {
  test.beforeAll(async () => {
    await ensureNoPasswordViaAPI();
  });

  test("the page that lost the session shows Use Here and is no longer connected", async ({
    browser,
  }) => {
    test.setTimeout(120_000);
    const context = await browser.newContext({ baseURL: process.env.JETKVM_URL });
    try {
      const [first, second] = [await context.newPage(), await context.newPage()];
      const useHere = (page: typeof first) => page.getByRole("button", { name: "Use Here" });
      const connected = (page: typeof first) =>
        page.evaluate(() => window.__kvmTestHooks?.isWebRTCConnected?.() ?? false);
      await first.goto("/", { waitUntil: "domcontentloaded" });
      await ensureRpcReady(first);

      await second.goto("/", { waitUntil: "domcontentloaded" });
      await waitForWebRTCReady(second);
      await expect(useHere(first)).toBeVisible({ timeout: 10_000 });
      await expect(useHere(second)).toBeHidden();
      await expect.poll(() => connected(first), { timeout: OLD_PEER_MS }).toBe(false);

      await useHere(first).click();
      await waitForWebRTCReady(first);
      await expect(useHere(second)).toBeVisible({ timeout: 10_000 });
      await expect(useHere(first)).toBeHidden();
      await expect.poll(() => connected(second), { timeout: OLD_PEER_MS }).toBe(false);
    } finally {
      await context.close();
    }
  });
});
