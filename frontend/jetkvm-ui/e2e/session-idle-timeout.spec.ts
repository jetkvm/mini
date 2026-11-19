import type { Page } from "@playwright/test";

import { test, expect, videoStops } from "./helpers/test";
import {
  HID_KEY,
  KNOWN_TEST_PASSWORDS,
  callJsonRpc,
  ensureNoPasswordViaAPI,
  ensureRpcReady,
  getDeviceHost,
  rawJsonRpc,
  sendAbsMouseMove,
  tapKey,
  waitForDeviceDown,
  waitForDeviceReady,
} from "./helpers";

const password = KNOWN_TEST_PASSWORDS[0];
const expiredLoginUrl = /\/login-local\?reason=session_expired$/;

function origin(): string {
  return new URL(process.env.JETKVM_URL!).origin;
}

async function enablePasswordWithTimeout(page: Page, minutes: number): Promise<void> {
  await ensureNoPasswordViaAPI();
  const res = await page.request.post(`${origin()}/auth/password-local`, {
    data: { password },
    headers: { Origin: origin() },
  });
  expect(res.ok(), "POST /auth/password-local").toBe(true);
  await ensureRpcReady(page, { navigateFirst: true });
  await callJsonRpc(page, "setLocalAuthSessionTimeout", { timeoutMinutes: minutes });
}

async function expectExpiredLoginPage(page: Page, timeout: number): Promise<void> {
  await expect(page).toHaveURL(expiredLoginUrl, { timeout });
  await expect(page.getByText("logged out after a period of inactivity")).toBeVisible();
  await expect(page.locator('input[name="password"]')).toBeVisible();
}

test.afterEach(async ({ page }) => {
  await page.goto("about:blank");
  const headers = { Origin: origin() };
  const login = await page.request.post(`${origin()}/auth/login-local`, {
    data: { password },
    headers,
  });
  if (!login.ok()) return;
  await ensureRpcReady(page, { navigateFirst: true });
  await callJsonRpc(page, "setLocalAuthSessionTimeout", { timeoutMinutes: 0 });
  await page.goto("about:blank");
  await page.request.delete(`${origin()}/auth/local-password`, { data: { password }, headers });
});

test("an idle KVM tab returns to the login page with the reason", async ({ page }) => {
  videoStops("the expired login closes the session");
  test.setTimeout(150_000);
  await enablePasswordWithTimeout(page, 1);

  await expectExpiredLoginPage(page, 100_000);
  const device = await page.request.get(`${origin()}/device`);
  expect(device.status(), "GET /device after expiry").toBe(401);
});

test("keyboard and mouse input keep the login alive", async ({ page }) => {
  test.setTimeout(150_000);
  await enablePasswordWithTimeout(page, 1);

  for (let i = 0; i < 5; i++) {
    await page.waitForTimeout(20_000);
    await tapKey(page, HID_KEY.LEFT_SHIFT);
    await sendAbsMouseMove(page, 1000 + i * 100, 1000);
  }
  await expect(page).toHaveURL(/\/$/);
  expect(await callJsonRpc(page, "getLocalAuthSessionTimeout")).toBe(1);
});

// The Mini keeps its login token in RAM only, so a restart ends the login
// with any timeout. Without the old token the Mini cannot tell an expired
// cookie from an unknown one, and the login page shows no reason.
test("a restart with the timeout on ends the login and keeps the timeout", async ({ page }) => {
  videoStops("reboots the device");
  test.setTimeout(240_000);
  await enablePasswordWithTimeout(page, 5);

  await Promise.all([
    rawJsonRpc(page, "reboot", { force: true }, 5000).catch(() => {
      // The reply can be lost in the reset; the device going down proves it.
    }),
    waitForDeviceDown(page, "device must go down after the reboot request", 30_000),
  ]);
  await waitForDeviceReady(getDeviceHost(), 90_000);
  await page.goto("/", { timeout: 60_000 });
  await expect(page).toHaveURL(/\/login-local/, { timeout: 60_000 });
  await expect(page.locator('input[name="password"]')).toBeVisible();

  const login = await page.request.post(`${origin()}/auth/login-local`, {
    data: { password },
    headers: { Origin: origin() },
  });
  expect(login.ok(), "POST /auth/login-local").toBe(true);
  await ensureRpcReady(page, { navigateFirst: true });
  expect(await callJsonRpc(page, "getLocalAuthSessionTimeout")).toBe(5);
});
