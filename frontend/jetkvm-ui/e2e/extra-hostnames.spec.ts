import { test, expect } from "./helpers/test";

import {
  callJsonRpc,
  ensureLocalAuthMode,
  skipWithoutCapability,
  waitForWebRTCReady,
} from "./helpers";

// A device that reports extra_hostnames takes other names for itself in the
// access settings, one per line. It stores them in lower case without empty
// lines, and refuses a list with an invalid name, keeping the stored one.

test("other names for the KVM are stored from the access settings, and an invalid one is refused", async ({
  page,
}) => {
  test.setTimeout(90_000);
  await ensureLocalAuthMode(page, { mode: "noPassword" });
  await waitForWebRTCReady(page);
  await skipWithoutCapability(page, "extra_hostnames");
  const original = (await callJsonRpc(page, "getExtraHostnames")) as string[];
  try {
    await page.goto("/settings/access");
    await waitForWebRTCReady(page);
    const names = page.getByPlaceholder("kvm.example.lan");
    await names.fill("KVM.Lab.Example\n\n rack-3 ");
    await page.getByRole("button", { name: "Save Names", exact: true }).click();
    await expect(names).toHaveValue("kvm.lab.example\nrack-3");
    expect(await callJsonRpc(page, "getExtraHostnames")).toEqual(["kvm.lab.example", "rack-3"]);

    await names.fill("kvm.lab.example\nbad_name");
    await page.getByRole("button", { name: "Save Names", exact: true }).click();
    await expect(page.getByText(/Failed to update names/)).toBeVisible();
    expect(await callJsonRpc(page, "getExtraHostnames")).toEqual(["kvm.lab.example", "rack-3"]);

    await page.reload();
    await waitForWebRTCReady(page);
    await expect(names).toHaveValue("kvm.lab.example\nrack-3");
  } finally {
    await callJsonRpc(page, "setExtraHostnames", { hostnames: original });
  }
});
