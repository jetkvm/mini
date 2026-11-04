import { mkdtempSync, truncateSync, writeFileSync } from "fs";
import { tmpdir } from "os";
import { join } from "path";
import type { Page } from "@playwright/test";
import { test, expect, videoStops } from "./helpers/test";
import { callJsonRpc, ensureNoPasswordViaAPI, ensureRpcReady } from "./helpers";

// Cancel Upload has to stop the request that is streaming the file, not just
// reset the view. What the device keeps of a stopped upload, and how the
// upload resumes, is tested without the page
// (tools/integration/tests/upload_cut_off.test.mjs).

const FILE_NAME = "e2e-upload-cancel.img";
// Large enough that the upload still runs when Cancel Upload is clicked.
const FILE_SIZE = 256 * 1024 * 1024;

// The device deletes nothing while the writer of a stopped upload is still
// at work, so the delete is asked for until the files are gone.
async function deleteImage(page: Page, name: string): Promise<void> {
  const names = [name, `${name}.incomplete`];
  await expect(async () => {
    const { files } = (await callJsonRpc(page, "listStorageFiles")) as {
      files: { filename: string }[];
    };
    const left = files.map(file => file.filename).filter(filename => names.includes(filename));
    for (const filename of left) await callJsonRpc(page, "deleteStorageFile", { filename });
    expect(left).toEqual([]);
  }).toPass({ timeout: 30_000 });
}

async function openUploadView(page: Page): Promise<void> {
  await page.goto("/mount", { waitUntil: "networkidle" });
  await ensureRpcReady(page);
  await page.getByText("JetKVM Storage Mount").click();
  await page.getByRole("button", { name: /^(next|continue)$/i }).click();
  // "Upload New Image" with an empty store, "Upload a new image" otherwise.
  await page.getByRole("button", { name: /^upload (a )?new image$/i }).click();
}

test.describe("Upload cancel", () => {
  let localPath = "";

  test.beforeAll(async ({ browser }) => {
    await ensureNoPasswordViaAPI();
    // A file without data blocks: it takes no space on this machine.
    localPath = join(mkdtempSync(join(tmpdir(), "jetkvm-e2e-")), FILE_NAME);
    writeFileSync(localPath, "");
    truncateSync(localPath, FILE_SIZE);
    const page = await browser.newPage();
    await ensureRpcReady(page, { navigateFirst: true });
    await deleteImage(page, FILE_NAME);
    await page.close();
  });

  test.afterAll(async ({ browser }) => {
    const page = await browser.newPage();
    await ensureRpcReady(page, { navigateFirst: true });
    await deleteImage(page, FILE_NAME);
    await page.close();
  });

  test("Cancel Upload stops the upload from the page", async ({ page }) => {
    test.setTimeout(90_000);

    await openUploadView(page);
    const capabilities = (await callJsonRpc(page, "getDeviceCapabilities")) as string[];
    if (capabilities.includes("wifi"))
      videoStops("the Wi-Fi Mini holds video off during an upload");

    await page.locator('input[type="file"]').setInputFiles(localPath);
    // The upload runs: the page shows that bytes have left the browser.
    const uploading = page.getByRole("heading", { name: /^Uploading / });
    const progress = uploading.locator("..").locator('div[style*="width:"]');
    await expect
      .poll(
        async () =>
          progress.evaluate(element => Number.parseFloat((element as HTMLElement).style.width)),
        { timeout: 20_000 },
      )
      .toBeGreaterThan(0);
    // Once the upload has run for a few seconds, the view shows the time left.
    await expect(page.getByTestId("upload-time-left")).toHaveText(
      /^(\d+ s|About \d+ min|About \d+ h \d+ min) left$/,
      { timeout: 15_000 },
    );

    const [request] = await Promise.all([
      page.waitForEvent("requestfailed", {
        predicate: failed => failed.url().includes("/storage/upload"),
        timeout: 20_000,
      }),
      page.getByRole("button", { name: "Cancel Upload" }).click(),
    ]);
    expect(request.failure()?.errorText, "the page ends the request of the upload").toBe(
      "net::ERR_ABORTED",
    );

    // The page leaves the upload for the list of the device's files, which
    // offers the partial file. The list shows five files per page, and the
    // button under it is there once the list has files.
    await expect(uploading).toBeHidden();
    await expect(page.getByRole("heading", { name: "Mount from JetKVM Storage" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Upload a new image" })).toBeVisible();
    const partial = page.locator("label", { hasText: `${FILE_NAME}.incomplete` });
    const next = page.getByRole("button", { name: "Next" });
    while (!(await partial.isVisible()) && (await next.isVisible()) && (await next.isEnabled()))
      await next.click();
    await expect(partial.getByRole("button", { name: "Continue uploading" })).toBeVisible();
  });
});
