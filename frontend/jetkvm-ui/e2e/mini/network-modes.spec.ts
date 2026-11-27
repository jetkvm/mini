import { test, expect } from "../helpers/test";
import { callJsonRpc, waitForWebRTCReady, withRpcPage } from "../helpers";

// The Mini refuses the modes of the shared UI that it cannot do: the mDNS
// mode "ipv6_only" and the IPv6 modes "static", "dhcpv6" and
// "slaac_and_dhcpv6". So the Network settings page must offer a mode only
// when the Mini stores it. tools/integration/tests/ipv6.test.mjs covers what
// the modes do; this spec covers the lists of the shipped page.

type NetworkSettings = Record<string, unknown>;

async function networkSettings(page: Parameters<typeof callJsonRpc>[0]): Promise<NetworkSettings> {
  return (await callJsonRpc(page, "getNetworkSettings")) as NetworkSettings;
}

let original: NetworkSettings | undefined;

// On a new session: the test's own session may be the one that failed.
test.afterAll(async ({ browser }) => {
  if (original === undefined) return;
  const settings = original;
  await withRpcPage(browser, async page => {
    const now = await networkSettings(page);
    if (now.mdns_mode !== settings.mdns_mode || now.ipv6_mode !== settings.ipv6_mode)
      await callJsonRpc(page, "setNetworkSettings", { settings });
  });
});

test("the Mini stores each mDNS mode and IPv6 mode that the Network settings page offers", async ({
  page,
}) => {
  test.setTimeout(180_000);
  await page.goto("/settings/network", { waitUntil: "networkidle" });
  // Not ensureRpcReady: its retries reload "/" and would leave this settings route.
  await waitForWebRTCReady(page, 60_000);
  original = await networkSettings(page);

  const capabilities = (await callJsonRpc(page, "getDeviceCapabilities")) as string[];
  for (const field of ["mdns_mode", "ipv6_mode"]) {
    // The page has the IPv6 list only for a device with IPv6.
    if (field === "ipv6_mode" && !capabilities.includes("ipv6")) continue;
    const select = page.locator(`select[name="${field}"]`);
    await expect(select, `the page has the ${field} list`).toBeVisible();
    const offered = await select
      .locator("option")
      .evaluateAll(options => options.map(option => (option as HTMLOptionElement).value));
    for (const mode of offered) {
      // The Mini answers a mode that it refuses with an error.
      await callJsonRpc(page, "setNetworkSettings", { settings: { ...original, [field]: mode } });
      expect((await networkSettings(page))[field], `the Mini stores ${field} "${mode}"`).toBe(mode);
    }
  }
});
