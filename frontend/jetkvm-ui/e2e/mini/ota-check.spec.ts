import { test, expect } from "../helpers/test";
import {
  createMockUpdateServer,
  configureDeviceUpdateUrl,
  restoreDeviceUpdateUrl,
  getOTAEnvVars,
  releaseFromEnv,
  installViaOta,
  type OTAEnvVars,
  openUpdatePage,
  ensureNoPasswordMode,
} from "./ota";

/**
 * What the update page shows for a check that the Mini refuses. The Mini's
 * answers to these checks are tested without the page, in
 * tools/integration/tests/update_check.test.mjs.
 */
test.describe("OTA update check (Mini)", () => {
  test.setTimeout(420000);

  let env: OTAEnvVars;

  test.beforeAll(async ({ browser }) => {
    test.setTimeout(420000);
    env = getOTAEnvVars();

    await ensureNoPasswordMode(browser);
    // These specs test the candidate's firmware and UI, whatever the earlier
    // projects left on the device.
    await installViaOta(browser, releaseFromEnv(env));
  });

  test("a failed check is shown as an error, not as up to date @ota", async ({ page, browser }) => {
    const mock = await createMockUpdateServer({
      component: "system",
      ...releaseFromEnv(env),
      failReleasesWith: 503,
    });
    try {
      await configureDeviceUpdateUrl(mock.url, { browser });
      await openUpdatePage(page);
      const outcome = page.getByText(/System is up to date|Update Error/);
      await expect(outcome).toBeVisible({ timeout: 60000 });
      expect(mock.releasesRequests().length, "device must have asked the mock").toBeGreaterThan(0);
      await expect(
        page.getByText("Update Error"),
        "a failed check is not 'up to date'",
      ).toBeVisible();
    } finally {
      await restoreDeviceUpdateUrl({ browser });
      await mock.close();
    }
  });

  test("a custom version the server does not have is shown as an error @ota", async ({
    page,
    browser,
  }) => {
    const mock = await createMockUpdateServer({ component: "system", ...releaseFromEnv(env) });
    try {
      await configureDeviceUpdateUrl(mock.url, { browser });
      await openUpdatePage(page, "custom_system_version=0.0.1&reset_config=false");
      await expect(page.getByText("Update Error")).toBeVisible({ timeout: 60000 });
    } finally {
      await restoreDeviceUpdateUrl({ browser });
      await mock.close();
    }
  });
});
