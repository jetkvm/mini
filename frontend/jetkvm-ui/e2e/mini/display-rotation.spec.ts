import { test, expect } from "../helpers/test";
import { callJsonRpc, waitForWebRTCReady } from "../helpers";

// The Display Orientation menu of the Hardware page lists the four quarter
// turns on a device that reports display_quarter_turns. Choosing one stores
// its setDisplayRotation value and turns the panel: getDisplayRotation and
// localDisplay.rotation in /webrtc/stats both report it. The menu label is the
// clockwise turn from upstream's Normal (270).

const HARDWARE = "/settings/hardware";
const TURNS = [
  { label: "90°", value: "0" },
  { label: "180°", value: "90" },
  { label: "270°", value: "180" },
  { label: "0°", value: "270" },
];

async function panelRotation(): Promise<unknown> {
  const response = await fetch(`${process.env.JETKVM_URL}/webrtc/stats`, {
    signal: AbortSignal.timeout(5000),
  });
  expect(response.ok, "/webrtc/stats answers").toBe(true);
  const stats = (await response.json()) as { localDisplay?: { rotation?: unknown } };
  return stats.localDisplay?.rotation;
}

test("the Display Orientation menu turns the local display in quarter turns", async ({ page }) => {
  test.setTimeout(120_000);
  await page.goto(HARDWARE);
  // Not ensureRpcReady: its retries reload "/" and would leave this settings route.
  await waitForWebRTCReady(page, 60_000);
  const rotation = async () =>
    ((await callJsonRpc(page, "getDisplayRotation")) as { rotation: string }).rotation;
  const original = await rotation();

  const menu = page.locator("select").filter({ has: page.locator('option[value="180"]') });
  await expect(menu.locator("option")).toHaveText(["0°", "90°", "180°", "270°"]);
  await expect(menu).toHaveValue(original);

  try {
    for (const turn of TURNS) {
      await menu.selectOption({ label: turn.label });
      await expect
        .poll(rotation, { message: `getDisplayRotation after ${turn.label}` })
        .toBe(turn.value);
      await expect
        .poll(panelRotation, { message: `localDisplay.rotation after ${turn.label}` })
        .toBe(Number(turn.value));
    }
    // The device keeps the choice: a new load of the page shows it.
    await menu.selectOption({ label: "180°" });
    await expect.poll(rotation).toBe("90");
    await page.reload();
    await waitForWebRTCReady(page, 60_000);
    await expect(menu).toHaveValue("90");
  } finally {
    await callJsonRpc(page, "setDisplayRotation", { params: { rotation: original } });
    expect(await rotation()).toBe(original);
  }
});
