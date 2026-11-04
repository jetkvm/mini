import { test, expect, videoStops } from "../helpers/test";
import {
  callJsonRpc,
  ensureNoPasswordViaAPI,
  ensureRpcReady,
  skipWithoutCapability,
} from "../helpers";

// The "Format as exFAT" action of the TF card in the storage view: its
// dialog shows the card's size and its count of images, Cancel keeps them,
// a refusal of the device stays in the dialog, and Format erases the card.
// The firmware's rules are an integration test
// (tools/integration/tests/storage_format.test.mjs). The test erases the
// card.

interface StorageSpace {
  bytesUsed: number;
  bytesFree: number;
}

const GIB = 1024 ** 3;

test("the TF card's format dialog shows the card and erases it on Format", async ({ page }) => {
  test.setTimeout(180_000);
  videoStops("the media dialog pauses video");
  await ensureNoPasswordViaAPI();
  await ensureRpcReady(page, { navigateFirst: true });
  await skipWithoutCapability(page, "removable_storage");
  const filename = `e2e-format-${Date.now()}.img`;
  await callJsonRpc(page, "unmountImage").catch(() => undefined);
  const start = (await callJsonRpc(page, "startStorageFileUpload", {
    filename,
    size: 512,
  })) as { dataChannel: string };
  const upload = await page.request.post(
    `/storage/upload?uploadId=${encodeURIComponent(start.dataChannel)}`,
    { data: Buffer.alloc(512) },
  );
  expect(upload.status()).toBe(200);
  const listed = async () =>
    ((await callJsonRpc(page, "listStorageFiles")) as { files: { filename: string }[] }).files;
  const files = (await listed()).length;
  const space = (await callJsonRpc(page, "getStorageSpace")) as StorageSpace;

  try {
    await page.getByRole("button", { name: "Virtual Media" }).click();
    await page.getByRole("button", { name: "Add New Media" }).click();
    await page.getByText("JetKVM Storage Mount").click();
    await page.getByRole("button", { name: "Continue" }).click();
    const action = page.getByRole("button", { name: "Format as exFAT" });
    const dialog = page.getByRole("dialog").filter({ hasText: "Format the TF card?" });
    const confirm = dialog.getByRole("button", { name: "Format", exact: true });
    // The dialog's root has no box of its own; its heading shows whether it is open.
    const title = dialog.getByRole("heading", { name: "Format the TF card?" });

    // The dialog names what is erased, and Cancel leaves it.
    await action.click();
    await expect(title).toBeVisible();
    await expect(dialog).toContainText("erases every file on the card");
    await expect(dialog.getByText("Images", { exact: true }).locator("xpath=..")).toContainText(
      String(files),
    );
    const size = (space.bytesUsed + space.bytesFree) / GIB;
    await expect(dialog.getByText("Card size", { exact: true }).locator("xpath=..")).toContainText(
      `${parseFloat(size.toFixed(2))} GB`,
    );
    await dialog.getByRole("button", { name: "Cancel" }).click();
    await expect(title).toBeHidden();
    expect((await listed()).map(file => file.filename)).toContain(filename);

    // The device refuses while an image from the card is mounted; the
    // dialog shows why and stays open.
    await callJsonRpc(page, "mountWithStorage", { filename, mode: "Disk" });
    await action.click();
    await confirm.click();
    await expect(dialog.getByRole("alert")).toContainText(
      "An image is mounted. Unmount it, then format the card.",
    );
    await expect(title).toBeVisible();
    await dialog.getByRole("button", { name: "Cancel" }).click();
    await callJsonRpc(page, "unmountImage");

    // Format erases the card, and the view shows it empty.
    await action.click();
    await confirm.click();
    await expect(page.getByText("TF card formatted as exFAT")).toBeVisible({ timeout: 120_000 });
    await expect(title).toBeHidden();
    expect(await listed()).toEqual([]);
    await expect(page.getByText("No images available")).toBeVisible();
  } finally {
    await callJsonRpc(page, "unmountImage").catch(() => undefined);
    await callJsonRpc(page, "deleteStorageFile", { filename }).catch(() => undefined);
  }
});
