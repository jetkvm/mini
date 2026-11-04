import { test, expect } from "../helpers/test";
import { callJsonRpc, skipWithoutRpc, waitForWebRTCReady } from "../helpers";

// The HDMI Sleep Mode checkbox of the Hardware page stores the delay after
// which the capture sleeps: upstream's 90 s when it is checked, and -1 (never)
// when it is cleared. What the Mini does with a delay is an integration test
// (tools/integration/tests/video_sleep.test.mjs).

const HARDWARE = "/settings/hardware";

interface VideoSleep {
  supported: boolean;
  enabled: boolean;
  duration: number;
}

test("the HDMI Sleep Mode checkbox stores a delay of 90 s and turns sleep off again", async ({
  page,
}) => {
  test.setTimeout(120_000);
  await page.goto(HARDWARE);
  // Not ensureRpcReady: its retries reload "/" and would leave this settings route.
  await waitForWebRTCReady(page, 60_000);
  await skipWithoutRpc(page, "getVideoSleepMode", "HDMI video sleep");
  const videoSleep = async () => (await callJsonRpc(page, "getVideoSleepMode")) as VideoSleep;
  const original = await videoSleep();
  expect(original.supported, "the Mini reports video sleep support").toBe(true);

  const checkbox = page
    .locator("label", { hasText: "HDMI Sleep Mode" })
    .locator('input[type="checkbox"]');
  // Clicks the checkbox the way a user does and waits until the device has
  // stored what the page sends.
  const setSleep = async (enabled: boolean) => {
    await expect(checkbox).toBeEnabled();
    if ((await checkbox.isChecked()) !== enabled) await checkbox.click();
    await expect(checkbox).toBeChecked({ checked: enabled });
    await expect
      .poll(async () => (await videoSleep()).duration >= 0, {
        message: `sleep ${enabled ? "enabled" : "disabled"} on the device`,
      })
      .toBe(enabled);
  };

  try {
    await setSleep(false);
    await setSleep(true);
    expect((await videoSleep()).duration, "the Hardware page sets 90 s").toBe(90);
    await setSleep(false);
    expect((await videoSleep()).duration, "a cleared checkbox stores no delay").toBeLessThan(0);
  } finally {
    await callJsonRpc(page, "setVideoSleepMode", { duration: original.duration });
    expect((await videoSleep()).duration).toBe(original.duration);
  }
});
