import { test, expect } from "../helpers/test";
import { waitForWebRTCReady } from "../helpers";

// With debug mode on, the Video settings page offers "Get Debugging Info",
// which asks the device for getVideoLogStatus and shows the Mini's video
// diagnostics. The report must name the capture mode, the quality factor, the
// transport, access units, sync IDRs and the memory policy.

const REQUIRED_LINES = [
  "mode: ready=true ",
  "quality-factor: ",
  "transport: started=true session=true peer=true state=connected",
  "access-units:",
  "sync-idr:",
  "memory-policy:",
];

test("the video debugging info shows the Mini's diagnostics", async ({ page }) => {
  test.setTimeout(90_000);
  // Debug mode is a browser setting (the persisted settings store); this
  // test's browser context is new, so nothing outside it changes.
  await page.addInitScript(() => {
    const key = "settings";
    let persisted: { state: Record<string, unknown>; version: number } = { state: {}, version: 0 };
    try {
      const stored = JSON.parse(localStorage.getItem(key) ?? "null");
      if (stored && typeof stored === "object") persisted = stored;
    } catch {
      // Not JSON: start from an empty store.
    }
    persisted.state = { ...persisted.state, debugMode: true };
    localStorage.setItem(key, JSON.stringify(persisted));
  });

  await page.goto("/settings/video", { waitUntil: "networkidle" });
  // Not ensureRpcReady: its retries reload "/" and would leave this settings route.
  await waitForWebRTCReady(page, 60_000);

  // The button's accessible name also carries its settings label.
  const button = page.locator("button", { hasText: "Get Debugging Info" });
  await expect(button).toBeEnabled();
  await button.click();
  const report = page.locator("pre").filter({ hasText: "JetKVM ESP32-P4 video diagnostics" });
  await expect(report).toBeVisible({ timeout: 15_000 });
  const text = (await report.textContent()) ?? "";
  expect(text.trim().startsWith("JetKVM ESP32-P4 video diagnostics")).toBe(true);
  for (const line of REQUIRED_LINES) expect(text, `the report has "${line}"`).toContain(line);
});
