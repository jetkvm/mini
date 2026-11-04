import type { Page } from "@playwright/test";
import { test, expect, videoStops } from "../helpers/test";
import { callJsonRpc, ensureRpcReady } from "../helpers";
import { agent, mountKey, registerSharedSession } from "./shared";

test.describe.configure({ mode: "serial" });

const MISSING_IMAGE_URL = "https://deb.debian.org/debian/jetkvm-missing-image.iso";
const MISSING_IMAGE_ERROR = /The URL is not available/;

let sharedPage: Page;
registerSharedSession(page => (sharedPage = page));

test.describe("Remote Host Agent: virtual media", () => {
  test("virtual-media: URL mount form shows unavailable image error", async () => {
    videoStops("the media dialog pauses video");
    test.setTimeout(30_000);

    try {
      await callJsonRpc(sharedPage, "unmountImage");
    } catch {
      /* ok if nothing mounted */
    }

    const baselineMounts = new Set((await agent!.getMounts()).map(mountKey));

    await sharedPage.getByRole("button", { name: "Virtual Media" }).click();
    await sharedPage.getByRole("button", { name: "Add New Media" }).click();
    await sharedPage.getByRole("button", { name: "Continue" }).click();
    await sharedPage.getByPlaceholder("https://example.com/image.iso").fill(MISSING_IMAGE_URL);
    await sharedPage.getByRole("button", { name: "Mount URL" }).click();

    await expect(sharedPage.getByRole("heading", { name: "Mount Error" })).toBeVisible();
    await expect(sharedPage.getByText(MISSING_IMAGE_ERROR)).toBeVisible();

    const stateAfter = (await callJsonRpc(sharedPage, "getVirtualMediaState")) as null | object;
    expect(stateAfter).toBeNull();

    await agent!.waitForMount(mount => !baselineMounts.has(mountKey(mount)), false, 5_000);

    await sharedPage.getByRole("button", { name: "Close" }).click();
    await ensureRpcReady(sharedPage);
  });
});
