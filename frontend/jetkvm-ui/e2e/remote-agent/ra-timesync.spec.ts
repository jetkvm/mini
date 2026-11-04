import type { Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
import { callJsonRpc, ensureRpcReady } from "../helpers";
import { registerSharedSession } from "./shared";

// The network settings page stores a custom NTP server. That the Mini then
// asks this server, and that the server survives a reboot, are integration
// tests (tools/integration/tests/ntp_server.test.mjs and
// settings_survive_reboot.test.mjs).

let page: Page;
let original: Record<string, unknown> | undefined;

test.afterEach(async ({ browser, baseURL }) => {
  test.setTimeout(90_000);
  if (!original) return;
  if (!page.isClosed()) await page.goto("about:blank");
  const recovery = await browser.newPage({ baseURL });
  try {
    await ensureRpcReady(recovery, { navigateFirst: true });
    await callJsonRpc(recovery, "setNetworkSettings", { settings: original });
    expect(await callJsonRpc(recovery, "getNetworkSettings")).toEqual(original);
  } finally {
    await recovery.close();
  }
});
registerSharedSession(p => {
  page = p;
});

test("a custom NTP server entered in the settings page is stored @network", async () => {
  page.setDefaultTimeout(10_000);
  const host = process.env.JETKVM_REMOTE_HOST!.split("@").at(-1)!;
  original = (await callJsonRpc(page, "getNetworkSettings")) as Record<string, unknown>;
  // Start from a different configuration, so that the page has to store the
  // server even when this host was already the selected one.
  await callJsonRpc(page, "setNetworkSettings", {
    settings: {
      ...original,
      time_sync_mode: "ntp_only",
      time_sync_ntp_servers: [],
      time_sync_http_urls: [],
      time_sync_disable_fallback: true,
    },
  });
  await page.waitForTimeout(2000); // allow deferred settings application to finish
  await page.goto("/settings/network");
  await ensureRpcReady(page);
  await page.locator('[name="time_sync_mode"]').selectOption("custom");
  const servers = page.locator('input[name^="time_sync_ntp_servers."]');
  await expect(servers.first()).toBeVisible();
  const entries = await servers.all();
  for (const entry of entries) await entry.fill(host);
  const configuredServers = entries.map(() => host);
  await page.getByRole("button", { name: "Save Settings", exact: true }).first().click();
  // NTP changes save directly; only connectivity changes prompt for confirmation.
  await expect
    .poll(() => callJsonRpc(page, "getNetworkSettings"))
    .toMatchObject({
      time_sync_mode: "custom",
      time_sync_ntp_servers: configuredServers,
      time_sync_disable_fallback: true,
    });
});
