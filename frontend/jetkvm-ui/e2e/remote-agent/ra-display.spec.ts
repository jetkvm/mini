import type { Page } from "@playwright/test";
import { test, expect, videoStops } from "../helpers/test";
import {
  callJsonRpc,
  waitForWebRTCReady,
  ensureRpcReady,
  waitForVideoDimensions,
  sshExec,
  skipWithoutDeviceShell,
  skipWithoutCapability,
} from "../helpers";
import { agent, registerSharedSession, remoteHostSetDPMS } from "./shared";

test.describe.configure({ mode: "serial" });

// 1366x768 at 60 Hz, with an 85.5 MHz pixel clock.
const EDID_1366x768 =
  "00ffffffffffff0028b401000100000001220103802213780aee95a3544c99260f50540000000101010101010101010101010101010166" +
  "2156aa51002030468f350058c21000001e000000fc004a65744b564d20313336367837000000fd00384c1e530a00202020202020200000" +
  "0010002020202020202020202020202000d0";

let sharedPage: Page;
registerSharedSession(page => (sharedPage = page));

test.describe("Remote Host Agent: display", () => {
  // ═══════════════════════════════════════════
  // HDMI SLEEP MODE
  // ═══════════════════════════════════════════

  test("hdmi-sleep: activates when no session and deactivates on reconnect @ssh", async () => {
    await skipWithoutDeviceShell();
    const SLEEP_MODE_SYSFS = "/sys/devices/platform/ff470000.i2c/i2c-4/4-000f/sleep_mode";

    const before = (await callJsonRpc(sharedPage, "getVideoSleepMode")) as {
      supported: boolean;
      duration: number;
    };

    if (!before.supported) {
      test.skip(true, "HDMI sleep mode not supported on this device");
      return;
    }

    const originalDuration = before.duration;

    // Set a very short sleep timer so the test doesn't wait long
    await callJsonRpc(sharedPage, "setVideoSleepMode", { duration: 3 });

    // Disconnect WebRTC by navigating the shared page away
    await sharedPage.goto("about:blank");

    // Wait for the 3s sleep timer + margin
    await new Promise(r => setTimeout(r, 5000));

    // Verify the HDMI capture chip entered sleep via sysfs
    const sleepState = (await sshExec(`cat ${SLEEP_MODE_SYSFS}`)).trim();
    expect(sleepState, "HDMI capture chip should be sleeping").toBe("1");

    // Reconnect — session start wakes the chip
    await sharedPage.goto("/", { waitUntil: "networkidle" });
    await waitForWebRTCReady(sharedPage);

    const wakeState = (await sshExec(`cat ${SLEEP_MODE_SYSFS}`)).trim();
    expect(wakeState, "HDMI capture chip should be awake after reconnect").toBe("0");

    // Restore original duration
    await callJsonRpc(sharedPage, "setVideoSleepMode", { duration: originalDuration });
  });

  test("video: non-aligned resolution 1366x768 produces video frames @custom-edid", async () => {
    videoStops("changes the EDID and host resolution");
    test.setTimeout(60_000);
    await skipWithoutCapability(sharedPage, "custom_edid");

    const originalEdid = (await callJsonRpc(sharedPage, "getEDID")) as string;

    await callJsonRpc(sharedPage, "setEDID", { edid: EDID_1366x768 }, 30000).catch(() => {});
    await new Promise(r => setTimeout(r, 3000));
    await sharedPage.goto("/", { waitUntil: "networkidle" });
    await ensureRpcReady(sharedPage);

    try {
      await agent!.waitForResolution("1366x768", 15_000);

      await expect
        .poll(
          async () => {
            const state = (await callJsonRpc(sharedPage, "getVideoState")) as {
              ready: boolean;
              width: number;
              height: number;
            };
            return state;
          },
          {
            message: "Waiting for KVM to report 1366x768",
            timeout: 15_000,
            intervals: [500, 1000],
          },
        )
        .toMatchObject({ ready: true, width: 1366, height: 768 });

      await sharedPage.goto("/", { waitUntil: "networkidle" });
      await waitForWebRTCReady(sharedPage);

      const dims = await waitForVideoDimensions(sharedPage, 15_000);
      expect(dims.width).toBe(1366);
      expect(dims.height).toBe(768);
    } finally {
      await callJsonRpc(sharedPage, "setEDID", { edid: originalEdid }, 30000).catch(() => {
        /* ignore */
      });

      await new Promise(r => setTimeout(r, 3000));

      await sharedPage.goto("/", { waitUntil: "networkidle" });
      await waitForWebRTCReady(sharedPage);
    }
  });

  // ═══════════════════════════════════════════
  // PANEL VISIBILITY: HIDE HEADER BAR / STATUS BAR
  // ═══════════════════════════════════════════

  test("panel-visibility: hide and show header and status bars via appearance settings", async () => {
    const checkboxFor = (label: string) => sharedPage.getByRole("checkbox", { name: label });

    const headerBar = sharedPage.locator('img[alt=""]').first();

    await sharedPage.evaluate(() => {
      const stored = localStorage.getItem("settings");
      if (stored) {
        const parsed = JSON.parse(stored);
        if (parsed.state) {
          delete parsed.state.hideHeaderBar;
          delete parsed.state.hideStatusBar;
          delete parsed.state.showHeaderBar;
          delete parsed.state.showStatusBar;
          localStorage.setItem("settings", JSON.stringify(parsed));
        }
      }
    });

    await sharedPage.goto("/", { waitUntil: "networkidle" });
    await waitForWebRTCReady(sharedPage);
    await expect(headerBar).toBeVisible({ timeout: 5000 });

    await sharedPage.goto("/settings/appearance", { waitUntil: "networkidle" });
    await checkboxFor("Hide header bar").check();

    await sharedPage.goto("/", { waitUntil: "networkidle" });
    await waitForWebRTCReady(sharedPage);
    await expect(headerBar).not.toBeVisible({ timeout: 5000 });

    await sharedPage.goto("/settings/appearance", { waitUntil: "networkidle" });
    await checkboxFor("Hide status bar").check();

    await sharedPage.goto("/", { waitUntil: "networkidle" });
    await waitForWebRTCReady(sharedPage);
    await expect(headerBar).not.toBeVisible({ timeout: 5000 });
    await expect(sharedPage.getByText("Caps Lock").first()).not.toBeVisible({ timeout: 5000 });

    await sharedPage.goto("/settings/appearance", { waitUntil: "networkidle" });
    await checkboxFor("Hide header bar").uncheck();
    await checkboxFor("Hide status bar").uncheck();

    await sharedPage.goto("/", { waitUntil: "networkidle" });
    await waitForWebRTCReady(sharedPage);
    await expect(headerBar).toBeVisible({ timeout: 5000 });
  });

  // ═══════════════════════════════════════════
  // HDMI SLEEP WAKE: SIGNAL RE-DETECTION AFTER DPMS OFF→ON
  // ═══════════════════════════════════════════

  test("hdmi-sleep-wake: re-detects signal after DPMS off→on with chip asleep @ssh", async () => {
    await skipWithoutDeviceShell();
    test.setTimeout(120_000);

    const SLEEP_MODE_SYSFS = "/sys/devices/platform/ff470000.i2c/i2c-4/4-000f/sleep_mode";

    const sleepInfo = (await callJsonRpc(sharedPage, "getVideoSleepMode")) as {
      supported: boolean;
      duration: number;
    };

    if (!sleepInfo.supported) {
      test.skip(true, "HDMI sleep mode not supported on this device");
      return;
    }

    const originalDuration = sleepInfo.duration;

    try {
      // Set a short sleep timer (3s) so the chip enters sleep quickly
      await callJsonRpc(sharedPage, "setVideoSleepMode", { duration: 3 });

      // Disconnect WebRTC so there are no active sessions → sleep timer starts
      await sharedPage.goto("about:blank");

      // Wait for sleep timer + margin
      await new Promise(r => setTimeout(r, 6000));

      // Verify chip entered sleep mode
      const sleepState = (await sshExec(`cat ${SLEEP_MODE_SYSFS}`)).trim();
      expect(sleepState, "Capture chip should be in sleep mode").toBe("1");

      // Toggle DPMS off on the remote host (simulates host GPU cutting signal)
      remoteHostSetDPMS(true);

      // Wait for the GPU to fully cut the TMDS clock
      await new Promise(r => setTimeout(r, 3000));

      // Bring the display back on
      remoteHostSetDPMS(false);

      // Wait for host display to stabilize
      await new Promise(r => setTimeout(r, 3000));

      // Reconnect — this triggers VideoStart() which must wake the chip and re-lock
      await sharedPage.goto("/", { waitUntil: "networkidle" });
      await waitForWebRTCReady(sharedPage);

      // Verify the chip woke up
      const wakeState = (await sshExec(`cat ${SLEEP_MODE_SYSFS}`)).trim();
      expect(wakeState, "Capture chip should be awake after reconnect").toBe("0");

      // Verify video state shows a valid signal (no error)
      const videoState = (await callJsonRpc(sharedPage, "getVideoState")) as {
        ready: boolean;
        error?: string;
        width: number;
        height: number;
      };
      expect(videoState.ready, `Video should be ready but got error: ${videoState.error}`).toBe(
        true,
      );
      expect(videoState.width).toBeGreaterThan(0);
      expect(videoState.height).toBeGreaterThan(0);
    } finally {
      // Always restore DPMS and sleep duration, even if test fails
      try {
        remoteHostSetDPMS(false);
      } catch {
        // best effort
      }

      // Reconnect if needed to restore sleep duration via RPC
      if (sharedPage.url() === "about:blank") {
        await sharedPage.goto("/", { waitUntil: "networkidle" });
        await waitForWebRTCReady(sharedPage);
      }
      await callJsonRpc(sharedPage, "setVideoSleepMode", { duration: originalDuration });
    }
  });
});
