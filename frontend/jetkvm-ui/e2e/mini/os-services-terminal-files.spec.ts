import type { BrowserContext, Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
import { ensureRpcReady } from "../helpers";
import { skipWithoutRemoteHost } from "../remote-agent/remote-agent";
import {
  TargetHost,
  bridgeSettings,
  connectSender,
  prepareOsServices,
  restoreOsServices,
  setBridgeSettings,
  waitForState,
  type OsServicesSnapshot,
} from "./os-services";
import {
  GUEST_HARNESS,
  fileBytes,
  offersTerminalAndFiles,
  sha256,
  shellQuote,
} from "./os-services-guest";

// What the page itself does with the OS Services terminal and file drop,
// with the sender on the remote host:
//
//   - the UI's terminal panel that ends without a closing line leaves a
//     notice;
//   - a file dropped on the video, through the UI's own handler, is written
//     even when the device's credit comes before the channel's open event.
//
// The channels without the page are integration tests
// (tools/integration/tests/os_services_terminal.test.mjs and
// os_services_files.test.mjs).
//
// The remote host needs the sender's user unit (ipkvm-os-services.service)
// and passwordless sudo. The spec deletes the file it wrote and puts back
// the OS Services settings, the approved list and the sender's state.

test.describe("OS Services terminal and file drop with the sender on the remote host", () => {
  test.describe.configure({ mode: "serial" });
  test.setTimeout(240_000);

  let context: BrowserContext | undefined;
  let page: Page;
  let target: TargetHost;
  let snapshot: OsServicesSnapshot | undefined;
  const written: string[] = [];

  test.beforeAll(async ({ browser }) => {
    test.setTimeout(180_000);
    skipWithoutRemoteHost();
    target = TargetHost.fromEnv();
    context = await browser.newContext({ baseURL: process.env.JETKVM_URL });
    page = await context.newPage();
    await page.addInitScript(GUEST_HARNESS);
    await ensureRpcReady(page, { timeoutMs: 90_000, navigateFirst: true });
    snapshot = await prepareOsServices(page, target);
    await connectSender(page, target, snapshot.fingerprint);
    const settings = await bridgeSettings(page);
    if (!settings.terminal_enabled || !settings.files_enabled)
      await setBridgeSettings(page, { terminal_enabled: true, files_enabled: true });
    await waitForState(
      page,
      "the sender offers a terminal and file drops",
      offersTerminalAndFiles,
      45_000,
    );
  });

  test.afterAll(async () => {
    test.setTimeout(180_000);
    try {
      for (const path of written)
        await target.run(`rm -f -- ${shellQuote(path)}`).catch(() => undefined);
      if (snapshot) await restoreOsServices(page, target, snapshot);
    } finally {
      await context?.close();
    }
  });

  test("the UI's terminal panel that ends without a closing line leaves a notice", async () => {
    // The first terminal the session offers is on the button, the others in
    // its menu, the guest terminal last.
    const guest = page.getByRole("button", { name: "Terminal", exact: true });
    const first = page.getByRole("button", { name: /^(KVM Terminal|USB Serial Console)$/ });
    await expect(guest.or(first), "the terminal button").toBeVisible();
    if (await guest.isVisible()) {
      await guest.click();
    } else {
      await first.locator("xpath=following-sibling::div[1]/button").click();
      await page.getByRole("menuitem", { name: "Terminal", exact: true }).click();
    }
    const panel = page.getByRole("heading", { name: "Terminal", exact: true });
    await expect(panel, "the panel opens").toBeVisible({ timeout: 20_000 });
    // The KVM closes a terminal it has no memory for without a line, and
    // nothing asks it to: the page closes the UI's channel, which the UI
    // sees as the same close.
    await page.evaluate(() => window.__e2eGuest!.closeNewestTerminal());
    await expect(panel, "the panel closes").toHaveCount(0);
    await expect(
      page.getByText("The terminal closed. The KVM may be short of memory; try again."),
      "the notice",
    ).toBeVisible();
  });

  test("a file dropped on the video is written when its credit comes before the open event", async () => {
    const file = {
      name: `e2e-guestfile-${Date.now()}-video.bin`,
      size: 100 * 1024 + 7,
      seed: 6,
    };
    await page.evaluate(() => window.__e2eGuest!.holdNextDropOpen(1000));
    // The UI's own drop handler on the video sends it (useGuestFileDrop).
    await page.evaluate(({ name, size, seed }) => {
      const transfer = new DataTransfer();
      transfer.items.add(new File([window.__e2eGuest!.fileBytes(seed, size)], name));
      document
        .querySelector("video")!
        .dispatchEvent(new DragEvent("drop", { dataTransfer: transfer, cancelable: true }));
    }, file);
    let done: { status?: string; path?: string } | undefined;
    await expect
      .poll(
        async () => {
          const messages = await page.evaluate(() => window.__e2eGuest!.heldDropMessages());
          done = messages.map(text => JSON.parse(text)).find(message => message.type === "done");
          return done?.status;
        },
        { message: "the device's done message", timeout: 60_000 },
      )
      .toBe("ok");
    written.push(done!.path!);
    expect(await target.sha256(done!.path!), "SHA-256 on the PC").toBe(
      sha256(fileBytes(file.seed, file.size)),
    );
  });
});
