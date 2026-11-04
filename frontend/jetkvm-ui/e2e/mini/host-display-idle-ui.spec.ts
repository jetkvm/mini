import { test, expect } from "../helpers/test";
import { callJsonRpc, skipWithoutRpc, waitForWebRTCReady, withRpcPage } from "../helpers";

// The Video settings checkbox "Hide JetKVM display when idle" sets the host
// display idle policy on the device. tools/integration/tests/host_display.test.mjs
// covers what the policy does to the host; this spec covers that the shipped
// checkbox sets it.

async function idleEnabled(page: Parameters<typeof callJsonRpc>[0]): Promise<boolean> {
  return ((await callJsonRpc(page, "getHostDisplayIdleMode")) as { enabled: boolean }).enabled;
}

let original: boolean | undefined;

// On a new session: the test's own session may be the one that failed.
test.afterAll(async ({ browser }) => {
  if (original === undefined) return;
  const enabled = original;
  await withRpcPage(browser, async page => {
    if ((await idleEnabled(page)) !== enabled)
      await callJsonRpc(page, "setHostDisplayIdleMode", { enabled });
  });
});

test("the idle-display checkbox sets the host display idle policy", async ({ page }) => {
  test.setTimeout(90_000);
  await page.goto("/settings/video", { waitUntil: "networkidle" });
  // Not ensureRpcReady: its retries reload "/" and would leave this settings route.
  await waitForWebRTCReady(page, 60_000);
  await skipWithoutRpc(page, "getHostDisplayIdleMode", "host display idle control");
  original = await idleEnabled(page);

  const checkbox = page
    .locator("label", { hasText: "Hide JetKVM display when idle" })
    .locator('input[type="checkbox"]');
  await expect(checkbox).toBeEnabled();
  await expect(checkbox).toBeChecked({ checked: original });
  for (const enabled of [!original, original]) {
    await checkbox.click();
    await expect
      .poll(() => idleEnabled(page), { message: `the device's policy is ${enabled}` })
      .toBe(enabled);
    await expect(checkbox).toBeChecked({ checked: enabled });
  }
});
