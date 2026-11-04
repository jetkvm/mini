import { test, expect } from "../helpers/test";

import { ensureRpcReady, waitForDeviceDown, waitForDeviceReady, getDeviceHost } from "../helpers";
import {
  createMockUpdateServer,
  configureDeviceUpdateUrl,
  restoreDeviceUpdateUrl,
  getLocalVersion,
  getOTAEnvVars,
  releaseFromEnv,
  baselineFromEnv,
  customVersionQuery,
  openUpdatePage,
  installViaOta,
  type MockUpdateServer,
  ensureNoPasswordMode,
} from "./ota";

// The update pauses video and reboots the device: video stops on purpose.
test.use({ videoWatch: false });

/**
 * After "Update Now" the page must stay on the update dialog until the
 * device announces its reboot, then show the rebooting overlay, poll the
 * health check and land on the update page of the new firmware. The device
 * must send willReboot before it reports updating=false, which the device
 * route takes as "update finished, reload the page".
 *
 * The reboot ordering belongs to the firmware that runs the install, so the
 * candidate is installed first and then installs the baseline, pinned through
 * the update page's custom version.
 *
 * Needs TEST_UPDATE_VERSION with RELEASE_STAGE_DIR or RELEASE_BINARY_PATH and
 * BASELINE_VERSION with BASELINE_STAGE_DIR or BASELINE_BINARY_PATH.
 */
test.describe("OTA reboot overlay", () => {
  test.setTimeout(600_000);

  let mock: MockUpdateServer | undefined;
  let target: string;

  test.beforeAll(async ({ browser }) => {
    // A candidate install is a full OTA (up to 300 s) plus the reboot.
    test.setTimeout(420_000);
    const env = getOTAEnvVars();
    const baseline = baselineFromEnv(env);
    if (!baseline) throw new Error("BASELINE_VERSION is required");
    target = baseline.version;

    await ensureNoPasswordMode(browser);
    await installViaOta(browser, releaseFromEnv(env));

    mock = await createMockUpdateServer({
      component: "system",
      ...releaseFromEnv(env),
      stable: baseline,
    });
    await configureDeviceUpdateUrl(mock.url, { browser });
  });

  test("the page shows the rebooting overlay and returns to the update page @ota", async ({
    page,
  }) => {
    await openUpdatePage(page, `${await customVersionQuery(target)}&reset_config=false`);

    const updateButton = page.locator('[data-testid="update-now-button"]');
    await expect(updateButton).toBeVisible({ timeout: 30_000 });
    await updateButton.click();

    await expect(page.getByText(/downloading|verifying|installing/i)).toBeVisible({
      timeout: 30_000,
    });

    await test.step("willReboot brings up the rebooting overlay, not a reload", async () => {
      // The install takes a minute or two. The
      // dialog's "Rebooting to complete the update" step lasts only until
      // willReboot arrives, which the firmware sends right after the install,
      // so wait for what follows it: the overlay, or a wrong reload.
      const overlay = page.getByText("Device is Rebooting");
      const reloaded = page.getByText("Update Completed Successfully");
      await expect(overlay.or(reloaded)).toBeVisible({ timeout: 300_000 });
      expect(
        await reloaded.isVisible().catch(() => false),
        "page reloaded with updateSuccess before the device announced its reboot",
      ).toBe(false);
      expect(page.url(), "page must not carry updateSuccess").not.toContain("updateSuccess");
      await expect(page.getByText("Waiting for device to restart")).toBeVisible({ timeout: 5_000 });
    });

    await test.step("Device goes down while the overlay is showing", async () => {
      await waitForDeviceDown(page, "device must reset after willReboot", 60_000);
      // The reset drops the session; the overlay must still be the view.
      await expect(
        page.getByText("Device is Rebooting").or(page.getByText("Unable to Reconnect")),
      ).toBeVisible({
        timeout: 5_000,
      });
    });

    await test.step("Health check redirects to the update page of the new firmware", async () => {
      const timedOut = page.getByText("Unable to Reconnect");
      let sawTimeout = false;
      await expect
        .poll(
          async () => {
            if (!sawTimeout && (await timedOut.isVisible().catch(() => false))) sawTimeout = true;
            return new URL(page.url()).pathname;
          },
          {
            message: "overlay must redirect once /device/status answers",
            timeout: 180_000,
            intervals: [1000],
          },
        )
        .toBe("/settings/general/update");
      if (sawTimeout) {
        test.info().annotations.push({
          type: "warning",
          description: "the overlay reported 'Unable to Reconnect' before the device came back",
        });
      }
      await waitForDeviceReady(getDeviceHost(), 120_000);
      await ensureRpcReady(page, { timeoutMs: 120_000 });
      expect(await getLocalVersion(page)).toBe(target);
    });
  });

  test.afterAll(async ({ browser }) => {
    test.setTimeout(120_000);
    if (mock) {
      await restoreDeviceUpdateUrl({ browser }).catch(() => undefined);
      await mock.close();
    }
  });
});
