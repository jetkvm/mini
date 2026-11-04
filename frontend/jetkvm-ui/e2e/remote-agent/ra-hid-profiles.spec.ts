import type { Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
import {
  callJsonRpc,
  ensureRpcReady,
  waitForWebRTCReady,
  skipWithoutRpc,
  withRpcPage,
} from "../helpers";
import { agent, registerSharedSession } from "./shared";
import { skipWithoutRemoteHost, waitForKeyboardReady } from "./remote-agent";

// A macro and a jiggler schedule entered in the settings UI are stored as
// entered and are shown again when the settings pages are opened anew, and
// the macro bar types the saved macro on the USB host, which is observed
// through the remote agent. That both survive a reboot, and that the jiggler
// moves the host's mouse while no session is connected, are integration tests
// (settings_survive_reboot.test.mjs and jiggler.test.mjs in
// tools/integration/tests).

// Linux input code: KEY_F24.
const KEY_F24 = 194;

interface JigglerConfig {
  inactivity_limit_seconds: number;
  jitter_percentage: number;
  schedule_cron_tab: string;
  timezone?: string;
}

// Every second, after one second without input, with no random delay.
const TEST_JIGGLER: JigglerConfig = {
  inactivity_limit_seconds: 1,
  jitter_percentage: 0,
  schedule_cron_tab: "* * * * * *",
  timezone: "UTC",
};
const macroName = `E2E F24 ${Date.now()}`;

let page: Page;
registerSharedSession(p => {
  page = p;
});
test.describe.configure({ mode: "serial" });

// Macros sorted by sortOrder, without the sortOrder numbers themselves.
function inOrder(macros: unknown): unknown[] {
  return [...(macros as { sortOrder: number }[])]
    .sort((a, b) => a.sortOrder - b.sortOrder)
    .map(({ sortOrder: _sortOrder, ...macro }) => macro);
}

// The list when this file starts, which includes the shared session's test
// macros; the delete step compares against it.
let originalMacros: unknown;
let originalMacroIds = new Set<string>();
let originalJigglerState: boolean | undefined;
let originalJigglerConfig: JigglerConfig | undefined;

test.beforeAll(async () => {
  // registerSharedSession skips without a remote host before it opens `page`,
  // and this hook still runs; skip here too.
  skipWithoutRemoteHost();
  await skipWithoutRpc(page, "getJigglerState", "mouse jiggler");
  originalMacros = await callJsonRpc(page, "getKeyboardMacros");
  // The cleanup removes only macros this file created: the ones with its name
  // and an id that was not on the device before.
  const existing = originalMacros as { id: string; name: string }[];
  originalMacroIds = new Set(existing.map(m => m.id));
  expect(
    existing.filter(m => m.name === macroName),
    `no macro is named ${macroName} yet`,
  ).toEqual([]);
  originalJigglerState = (await callJsonRpc(page, "getJigglerState")) as boolean;
  originalJigglerConfig = (await callJsonRpc(page, "getJigglerConfig")) as JigglerConfig;
});

// On the shared session's page while it is open: the device serves one
// session, and a second page would take it over. The shared session's own
// afterAll closes that page, so if it ran first this hook opens its own.
test.afterAll(async ({ browser }) => {
  if (originalJigglerConfig === undefined || originalJigglerState === undefined) return;
  const restore = async (p: Page) => {
    // Each step runs even if another fails; all failures are reported.
    const errors: unknown[] = [];
    const attempt = async (step: () => Promise<unknown>) => {
      try {
        await step();
      } catch (error) {
        errors.push(error);
      }
    };
    // Remove only this file's macro. registerSharedSession restores the list
    // it changed; writing originalMacros back here would restore its test
    // macros too, whichever afterAll runs first.
    await attempt(async () => {
      const macros = (await callJsonRpc(p, "getKeyboardMacros")) as {
        id: string;
        name: string;
      }[];
      const created = (m: { id: string; name: string }) =>
        m.name === macroName && !originalMacroIds.has(m.id);
      if (macros.some(created)) {
        const rest = macros.filter(m => !created(m));
        await callJsonRpc(p, "setKeyboardMacros", { params: { macros: rest } });
      }
    });
    await attempt(() =>
      callJsonRpc(p, "setJigglerConfig", { jigglerConfig: originalJigglerConfig }),
    );
    await attempt(() => callJsonRpc(p, "setJigglerState", { enabled: originalJigglerState }));
    if (errors.length) throw new AggregateError(errors, "hid-profiles cleanup failed");
  };
  if (page && !page.isClosed()) {
    await ensureRpcReady(page, { navigateFirst: true });
    await restore(page);
  } else {
    await withRpcPage(browser, restore);
  }
});

function jigglerSelect(p: Page) {
  return p.locator("select").filter({ has: p.locator('option[value="custom"]') });
}

// The jiggler form labels are not bound to their inputs; the input is a
// sibling of the label inside the same field wrapper.
function inputLabelled(p: Page, label: string) {
  return p.locator("label", { hasText: label }).locator("xpath=..").locator("input").first();
}

async function openSettings(p: Page, section: "macros" | "mouse"): Promise<void> {
  await p.goto(`/settings/${section}`, { waitUntil: "networkidle" });
  // Not ensureRpcReady: its retries reload "/" and would leave this settings route.
  await waitForWebRTCReady(p, 60_000);
  if (section === "macros") {
    await expect(
      p.getByRole("button", { name: "Add new macro", exact: true }).first(),
    ).toBeVisible();
  } else {
    await expect(jigglerSelect(p)).toBeVisible();
  }
}

async function jigglerSnapshot(p: Page) {
  return {
    state: (await callJsonRpc(p, "getJigglerState")) as boolean,
    config: (await callJsonRpc(p, "getJigglerConfig")) as JigglerConfig,
  };
}

async function macrosNamed(p: Page, name: string) {
  const macros = (await callJsonRpc(p, "getKeyboardMacros")) as {
    name: string;
    steps: unknown;
  }[];
  return macros.filter(macro => macro.name === name);
}

test("a macro and a jiggler schedule entered in the UI are saved as entered", async () => {
  test.setTimeout(90_000);

  await test.step("add an F24 macro", async () => {
    await openSettings(page, "macros");
    await page.getByRole("button", { name: "Add new macro", exact: true }).first().click();
    await expect(page.getByText("Add New Macro").first()).toBeVisible();
    await page.getByPlaceholder("Macro Name").fill(macroName);
    await page.getByPlaceholder("Search for key…").fill("F24");
    await page.getByRole("option", { name: "F24", exact: true }).click();
    await page.getByRole("button", { name: "Save Macro" }).click();
    await expect(page.getByRole("heading", { name: macroName, exact: true })).toBeVisible({
      timeout: 12_000,
    });
    expect(await macrosNamed(page, macroName)).toEqual([
      expect.objectContaining({ steps: [{ keys: ["F24"], modifiers: [], delay: 50 }] }),
    ]);
  });

  await test.step("set a custom jiggler schedule", async () => {
    await openSettings(page, "mouse");
    await jigglerSelect(page).selectOption("custom");
    await page.getByPlaceholder("*/20 * * * * *").fill(TEST_JIGGLER.schedule_cron_tab);
    await inputLabelled(page, "Inactivity Limit Seconds").fill(
      String(TEST_JIGGLER.inactivity_limit_seconds),
    );
    await inputLabelled(page, "Random delay").fill(String(TEST_JIGGLER.jitter_percentage));
    const timezone = page
      .locator("select")
      .filter({ has: page.locator('option[value="UTC"]') })
      .filter({ hasNot: page.locator('option[value="custom"]') });
    await expect(timezone).toBeEnabled();
    await timezone.selectOption("UTC");
    await page.getByRole("button", { name: "Save Jiggler Config" }).click();
    await expect
      .poll(() => jigglerSnapshot(page), { timeout: 12_000 })
      .toEqual({ state: true, config: TEST_JIGGLER });
  });

  await test.step("the settings pages show both when they are opened again", async () => {
    await openSettings(page, "macros");
    await expect(page.getByRole("heading", { name: macroName, exact: true })).toBeVisible();
    await openSettings(page, "mouse");
    await expect(jigglerSelect(page)).toHaveValue("custom");
    await expect(page.getByPlaceholder("*/20 * * * * *")).toHaveValue(
      TEST_JIGGLER.schedule_cron_tab,
    );
    await expect(inputLabelled(page, "Inactivity Limit Seconds")).toHaveValue(
      String(TEST_JIGGLER.inactivity_limit_seconds),
    );
    await expect(inputLabelled(page, "Random delay")).toHaveValue(
      String(TEST_JIGGLER.jitter_percentage),
    );
  });
});

test("the macro bar sends the saved macro to the host", async () => {
  test.setTimeout(90_000);
  await ensureRpcReady(page, { navigateFirst: true });
  await waitForKeyboardReady(agent!, page, 30_000);

  const button = page.getByRole("button", { name: macroName, exact: true });
  await expect(button).toBeVisible();
  await agent!.clearKeyboardEvents();
  await button.click();
  // One press, then one release: a release first, or a press without one,
  // would leave the host with a held key.
  await expect
    .poll(
      async () =>
        (await agent!.getKeyboardEvents())
          .filter(event => event.code === KEY_F24)
          .map(event => event.type)
          .filter(type => type === "key_press" || type === "key_release"),
      { message: "F24 press, then release, on the host", timeout: 5_000, intervals: [100] },
    )
    .toEqual(["key_press", "key_release"]);
});

test("deleting the macro and disabling the jiggler in the UI take effect", async () => {
  test.setTimeout(60_000);

  await openSettings(page, "macros");
  await page.getByRole("button", { name: `Delete macro ${macroName}`, exact: true }).click();
  await expect(page.getByText("Delete Macro").first()).toBeVisible();
  await page.getByRole("button", { name: "Delete", exact: true }).click();
  await expect(page.getByRole("heading", { name: macroName, exact: true })).toHaveCount(0, {
    timeout: 12_000,
  });
  // The UI keeps the remaining macros' sortOrder values after a delete; their
  // order and content must be the original ones.
  expect(
    inOrder(await callJsonRpc(page, "getKeyboardMacros")),
    "the original macros, in their order",
  ).toEqual(inOrder(originalMacros));

  await openSettings(page, "mouse");
  await jigglerSelect(page).selectOption("disabled");
  await expect.poll(() => callJsonRpc(page, "getJigglerState"), { timeout: 10_000 }).toBe(false);
});
