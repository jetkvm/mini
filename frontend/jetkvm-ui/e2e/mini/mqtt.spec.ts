import type { Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
import { callJsonRpc, skipWithoutRpc, waitForWebRTCReady, withRpcPage } from "../helpers";
import { startMqttBroker } from "../../../../tools/integration/lib/mqtt_broker.mjs";
import { brokerHost } from "./mqtt-broker";
import { recordRpc, rpcLog } from "./rpc-recorder";

// The MQTT settings page against a broker on this host's address towards the
// device: the page tests the connection with the values of its fields, and
// "Save & Reconnect" stores them and connects the device. What the device
// does on the broker afterwards is tested without the page, in
// tools/integration/tests/mqtt.test.mjs.

const SETTING_KEYS = [
  "enabled",
  "broker",
  "port",
  "username",
  "password",
  "base_topic",
  "use_tls",
  "tls_insecure",
  "enable_ha_discovery",
  "enable_actions",
  "debounce_ms",
] as const;
type MqttSettings = Record<(typeof SETTING_KEYS)[number], unknown>;

async function mqttSettings(page: Page): Promise<MqttSettings> {
  const value = (await callJsonRpc(page, "getMqttSettings")) as Record<string, unknown>;
  for (const key of SETTING_KEYS)
    expect(value, `getMqttSettings reports ${key}`).toHaveProperty(key);
  return Object.fromEntries(SETTING_KEYS.map(key => [key, value[key]])) as MqttSettings;
}

async function mqttConnected(page: Page): Promise<boolean> {
  const status = (await callJsonRpc(page, "getMqttStatus")) as { connected?: unknown };
  expect(typeof status.connected, "getMqttStatus reports connected").toBe("boolean");
  return status.connected as boolean;
}

/** What this test uses of the broker of tools/integration/lib/mqtt_broker.mjs. */
interface Broker {
  port: number;
  /** How many times the client with this ID has connected. */
  connects(clientId: string): number;
  close(): Promise<unknown>;
}

function setting(page: Page, title: string) {
  return page.locator("label").filter({ has: page.getByText(title, { exact: true }) });
}

test.describe("MQTT settings page (Mini)", () => {
  test.setTimeout(120_000);

  let broker: Broker | undefined;
  let original: MqttSettings | undefined;

  test.beforeAll(async ({ browser }) => {
    await withRpcPage(browser, async page => {
      await skipWithoutRpc(page, "getMqttSettings", "MQTT");
      original = await mqttSettings(page);
    });
  });

  // A safety net for a failed or timed-out test; the test restores and
  // checks the same settings itself as its last step.
  test.afterAll(async ({ browser }) => {
    try {
      if (original) {
        await withRpcPage(browser, async page => {
          await callJsonRpc(page, "setMqttSettings", { settings: original });
        });
      }
    } finally {
      await broker?.close();
    }
  });

  test("the page tests the connection, stores the settings and connects", async ({ page }) => {
    const host = await brokerHost(process.env.JETKVM_URL!);
    broker = (await startMqttBroker({ host })) as Broker;
    const b = broker;

    const pageErrors: string[] = [];
    page.on("pageerror", error => pageErrors.push(error.message));
    page.on("console", message => {
      if (message.type() === "error") pageErrors.push(`console: ${message.text()}`);
    });
    await recordRpc(page);
    // ensureRpcReady would reload "/" on a retry and leave this route, so the
    // page waits for its own session here.
    await page.goto("/settings/mqtt", { waitUntil: "networkidle" });
    await waitForWebRTCReady(page, 60_000);
    await expect(page.getByText("Enable MQTT", { exact: true })).toBeVisible();

    const deviceId = (await callJsonRpc(page, "getDeviceID")) as string;
    expect(deviceId, "getDeviceID").toBeTruthy();
    const clientId = `jetkvm-${deviceId}`;
    const testClientId = `${clientId}-test`;
    const configuredBase = `mini-e2e-${(Date.now() >>> 0).toString(16).padStart(8, "0")}`;
    const target: MqttSettings = {
      ...original!,
      enabled: true,
      broker: host,
      port: b.port,
      base_topic: configuredBase,
      use_tls: false,
      tls_insecure: false,
      enable_ha_discovery: true,
      enable_actions: true,
    };

    await test.step("the page tests the connection against the broker", async () => {
      await setting(page, "Enable MQTT").locator('input[type="checkbox"]').setChecked(true);
      await setting(page, "Broker Address").locator("input").fill(host);
      await setting(page, "Use TLS").locator('input[type="checkbox"]').setChecked(false);
      await setting(page, "Port").locator("select").selectOption("custom");
      await page.locator('input[type="number"][placeholder="1883"]').fill(String(b.port));
      await setting(page, "Remote Control").locator('input[type="checkbox"]').setChecked(true);
      await setting(page, "Auto-Discovery").locator('input[type="checkbox"]').setChecked(true);
      await setting(page, "Base Topic").locator("select").selectOption("custom");
      await page.getByPlaceholder("jetkvm", { exact: true }).fill(configuredBase);

      const tests = (await rpcLog(page)).successes("testMqttConnection");
      await page.getByRole("button", { name: "Test Connection", exact: true }).click();
      await expect
        .poll(async () => (await rpcLog(page)).successes("testMqttConnection"))
        .toBeGreaterThan(tests);
      await expect
        .poll(() => b.connects(testClientId), { timeout: 5000 })
        .toBeGreaterThanOrEqual(1);
      await expect(page.getByText("Connection successful")).toBeVisible();
    });

    await test.step("Save & Reconnect stores the settings and connects", async () => {
      const log = await rpcLog(page);
      const sets = log.successes("setMqttSettings");
      const tests = log.successes("testMqttConnection");
      await page.getByRole("button", { name: "Save & Reconnect", exact: true }).click();
      await expect
        .poll(async () => (await rpcLog(page)).successes("setMqttSettings"), { timeout: 15_000 })
        .toBeGreaterThan(sets);
      // Saving tests the connection first.
      expect((await rpcLog(page)).successes("testMqttConnection")).toBeGreaterThan(tests);
      await expect
        .poll(() => b.connects(testClientId), { timeout: 5000 })
        .toBeGreaterThanOrEqual(2);
      await expect.poll(() => b.connects(clientId), { timeout: 10_000 }).toBeGreaterThanOrEqual(1);
      await expect.poll(() => mqttConnected(page), { timeout: 15_000 }).toBe(true);
      expect(await mqttSettings(page), "settings round-trip").toEqual(target);
    });

    await test.step("restoring the original settings", async () => {
      await callJsonRpc(page, "setMqttSettings", { settings: original });
      await expect.poll(() => mqttSettings(page), { timeout: 10_000 }).toEqual(original);
    });

    await test.step("no page or RPC errors", async () => {
      const log = await rpcLog(page);
      expect(log.errors(), "RPC errors on the MQTT page").toEqual([]);
      expect(log.rejections(), "unhandled rejections on the MQTT page").toEqual([]);
      expect(pageErrors, "script and console errors on the MQTT page").toEqual([]);
      expect(log.requested(), "a Mini has no extension port").not.toContain("getActiveExtension");
    });
  });
});
