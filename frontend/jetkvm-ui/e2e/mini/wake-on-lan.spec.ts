import type { Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
import { callJsonRpc, ensureNoPasswordViaAPI, ensureRpcReady, skipWithoutRpc } from "../helpers";

// Saved Wake-on-LAN targets, driven through the shipped UI: a target added in
// the popover is stored on the device, and the popover's delete removes it
// again. The test never sends a magic packet; the target is inert. That a
// stored target survives a reboot is an integration test
// (tools/integration/tests/settings_survive_reboot.test.mjs).

interface WolDevice {
  name: string;
  macAddress: string;
  broadcastIP?: string;
}

function normalized(value: unknown): WolDevice[] {
  if (!Array.isArray(value)) throw new Error(`getWakeOnLanDevices: ${JSON.stringify(value)}`);
  return value.map((item: WolDevice) => ({
    name: String(item.name ?? ""),
    macAddress: String(item.macAddress ?? ""),
    ...(item.broadcastIP ? { broadcastIP: String(item.broadcastIP) } : {}),
  }));
}

async function getDevices(page: Page): Promise<WolDevice[]> {
  return normalized(await callJsonRpc(page, "getWakeOnLanDevices"));
}

async function openPopover(page: Page): Promise<void> {
  await page.getByRole("button", { name: "Wake on LAN", exact: true }).click();
  await expect(page.getByRole("button", { name: "Add New Device" })).toBeVisible();
}

async function closePopover(page: Page): Promise<void> {
  await page.getByRole("button", { name: "Close", exact: true }).click();
  await expect(page.getByRole("button", { name: "Add New Device" })).toBeHidden();
}

const stamp = new Date().toISOString().replace(/\D/g, "");
// A locally administered unicast MAC and a documentation-range broadcast
// address: valid for the device, never a real target.
const target: WolDevice = {
  name: `Mini E2E ${stamp.slice(4, 8)}-${stamp.slice(8, 14)}`,
  macAddress: `02:16:07:${stamp.slice(8, 10)}:${stamp.slice(10, 12)}:${stamp.slice(12, 14)}`,
  broadcastIP: "192.0.2.255",
};

test.describe.configure({ mode: "serial" });

test.describe("Wake-on-LAN saved targets", () => {
  let page: Page;
  let original: WolDevice[] | undefined;

  test.beforeAll(async ({ browser }) => {
    await ensureNoPasswordViaAPI();
    page = await browser.newPage();
    await ensureRpcReady(page, { navigateFirst: true });
    await skipWithoutRpc(page, "getWakeOnLanDevices", "Wake-on-LAN");
    original = await getDevices(page);
    expect(original.length, "the device must have room for one more target").toBeLessThan(16);
  });

  test.afterAll(async () => {
    if (!page) return;
    try {
      if (original) {
        await ensureRpcReady(page, { navigateFirst: true });
        if (JSON.stringify(await getDevices(page)) !== JSON.stringify(original)) {
          await callJsonRpc(page, "setWakeOnLanDevices", { params: { devices: original } });
        }
        expect(await getDevices(page), "the original target list must be restored").toEqual(
          original,
        );
      }
    } finally {
      await page.close();
    }
  });

  test("a target added in the UI is stored and the UI deletes it", async () => {
    await test.step("add the target in the popover", async () => {
      await openPopover(page);
      await page.getByRole("button", { name: "Add New Device" }).click();
      await page.getByPlaceholder("Plex Media Server").fill(target.name);
      await page.getByPlaceholder("00:b0:d0:63:c2:26").fill(target.macAddress);
      await page
        .locator("select")
        .filter({ has: page.locator('option[value="custom"]') })
        .selectOption("custom");
      await page.getByPlaceholder("192.168.1.255").fill(target.broadcastIP!);
      const save = page.getByRole("button", { name: "Save Device" });
      await expect(save).toBeEnabled();
      await save.click();
      await expect(page.getByText(target.name, { exact: true })).toBeVisible();
      expect(await getDevices(page)).toEqual([...original!, target]);
      await closePopover(page);
    });

    await test.step("delete the target in the popover", async () => {
      await openPopover(page);
      const row = page
        .locator("div")
        .filter({ has: page.getByText(target.name, { exact: true }) })
        .filter({ has: page.getByRole("button", { name: "Delete device" }) })
        .last();
      await row.getByRole("button", { name: "Delete device" }).click();
      await page.getByRole("button", { name: "Delete", exact: true }).click();
      await expect(page.getByText(target.name, { exact: true })).toBeHidden();
      expect(await getDevices(page)).toEqual(original);
      await closePopover(page);
    });
  });
});
