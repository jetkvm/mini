import { test, expect } from "../helpers/test";
import { callJsonRpc, ensureNoPasswordViaAPI, waitForWebRTCReady } from "../helpers";
import { recordRpc, rpcLog } from "./rpc-recorder";

// Read-only audit of every settings page the UI links to. The test clicks
// only the settings navigation, never a control, and fails on anything the
// browser or the device reports as an error while a page loads: uncaught
// exceptions, unhandled rejections, console errors, failed requests, JSON-RPC
// error responses and requests the device never answers. The route list comes
// from the navigation itself, so a page the UI adds or hides is audited
// without editing this spec.
//
// A "method not found" answer counts as an error too: the UI gates optional
// features on getDeviceCapabilities, so a page that calls a method the device
// does not implement has a wrong gate.

// Getters each page must call and get an answer to while it is shown: every
// getter the page sends on a Mini, except the upstream probes. A page that
// renders its heading but stops loading its data would otherwise pass with
// defaults on screen. The appearance and macros pages send none; the macros
// page shows the list the session loaded when it connected.
// Events the device sends by itself when the rpc channel opens; the first
// page sees the session's connect.
const REQUIRED_EVENTS: Record<string, string[]> = {
  "/settings/general": ["localVersion", "deviceCapabilities"],
};

const REQUIRED_RPCS: Record<string, string[]> = {
  // The first page also covers what the device session asks for when it
  // loads (video, keyboard and macro state).
  "/settings/general": [
    "getAutoUpdateState",
    "getAudioConfig",
    "getKeyDownState",
    "getKeyboardLedState",
    "getKeyboardMacros",
    "getNetworkSettings",
    "getUsbDevices",
    "getVideoState",
  ],
  "/settings/mouse": ["getJigglerState"],
  "/settings/keyboard": ["getKeyboardLayout"],
  "/settings/video": [
    "getEDID",
    "getEDIDPresets",
    "getHostDisplayIdleMode",
    "getStreamQualityFactor",
    "getSupportedVideoCodecs",
    "getVideoCodecPreference",
  ],
  "/settings/audio": ["getAudioConfig", "getUsbDevices"],
  "/settings/hardware": [
    "getBacklightSettings",
    "getDeviceID",
    "getDisplayRotation",
    "getUsbConfig",
    "getUsbDevices",
    "getVideoSleepMode",
  ],
  "/settings/access": ["getCloudState", "getDeviceID"],
  "/settings/network": [
    "getCloudState",
    "getMacAddressSettings",
    "getNetworkSettings",
    "getNetworkState",
  ],
  "/settings/mqtt": ["getMqttSettings", "getMqttStatus"],
  "/settings/advanced": ["getDevChannelState", "getUsbEmulationState"],
};
// Only a device that reports os_services lists the OS Services page.
const OS_SERVICES_RPCS: Record<string, string[]> = {
  "/settings/os-services": ["getOsServicesSettings"],
};
// Only a device that reports tailscale lists the Tailscale page.
const TAILSCALE_RPCS: Record<string, string[]> = {
  "/settings/tailscale": ["getTailscaleStatus"],
};
// Pages the navigation must also list; they send no getters of their own.
const ERROR_ONLY_ROUTES = ["/settings/appearance", "/settings/macros"];

// Memory-policy counters in /webrtc/stats memoryBudget. They are cumulative
// since boot and run.sh runs other specs first, so the audit compares them
// before and after instead of requiring zero.
const MEMORY_COUNTERS = [
  "admissionDenials",
  "videoRebuildBeginDenials",
  "videoRebuildFailures",
  "jsonAllocationFailures",
] as const;

async function deviceStats(): Promise<Record<string, unknown>> {
  const response = await fetch(`${process.env.JETKVM_URL}/webrtc/stats`, {
    signal: AbortSignal.timeout(5000),
  });
  if (!response.ok) throw new Error(`/webrtc/stats returned ${response.status}`);
  return (await response.json()) as Record<string, unknown>;
}

function memoryCounters(stats: Record<string, unknown>): Record<string, number> {
  const budget = (stats.memoryBudget ?? {}) as Record<string, unknown>;
  return Object.fromEntries(
    MEMORY_COUNTERS.map(name => {
      const value = budget[name];
      if (typeof value !== "number")
        throw new Error(`memoryBudget.${name} missing: ${JSON.stringify(value)}`);
      return [name, value];
    }),
  );
}

test("every settings page loads without browser, request or RPC errors", async ({ page }) => {
  test.setTimeout(240_000);
  await ensureNoPasswordViaAPI();
  const before = memoryCounters(await deviceStats());

  // Problems carry the route that was shown when they happened.
  let route = "/settings/general";
  const problems: { route: string; kind: string; detail: string }[] = [];
  page.on("pageerror", error =>
    problems.push({ route, kind: "page error", detail: error.message }),
  );
  page.on("console", message => {
    if (message.type() === "error")
      problems.push({ route, kind: "console error", detail: message.text() });
  });
  const origin = new URL(process.env.JETKVM_URL!).origin;
  page.on("response", response => {
    if (new URL(response.url()).origin === origin && !response.ok())
      problems.push({ route, kind: "request", detail: `${response.status()} ${response.url()}` });
  });
  page.on("requestfailed", request => {
    const failure = request.failure()?.errorText ?? "failed";
    // A request the page cancels itself (unmount, superseded query) is not a failure.
    if (new URL(request.url()).origin === origin && failure !== "net::ERR_ABORTED")
      problems.push({ route, kind: "request", detail: `${failure} ${request.url()}` });
  });
  await recordRpc(page);

  await page.goto(route);
  // Not ensureRpcReady: its retries reload "/", and the routes below are read
  // from the settings navigation, which only this page shows.
  await waitForWebRTCReady(page, 60_000);
  await expect(page.getByRole("heading").filter({ hasText: /\S/ }).first()).toBeVisible();

  const capabilities = (await callJsonRpc(page, "getDeviceCapabilities")) as string[];
  const getters: Record<string, string[]> = {
    ...REQUIRED_RPCS,
    ...(capabilities.includes("os_services") ? OS_SERVICES_RPCS : {}),
    ...(capabilities.includes("tailscale") ? TAILSCALE_RPCS : {}),
    // Only a device that reports usb_debugging shows the USB Debugging switch.
    ...(capabilities.includes("usb_debugging")
      ? {
          "/settings/advanced": [...REQUIRED_RPCS["/settings/advanced"], "getUsbDebuggingState"],
        }
      : {}),
  };
  // The navigation shows OS Services and Tailscale once the
  // deviceCapabilities event has arrived, which can be after WebRTC is ready.
  let routes: string[] = [];
  await expect
    .poll(
      async () => {
        routes = await page.evaluate(() => {
          const paths = [...document.querySelectorAll<HTMLAnchorElement>("a[href]")]
            .filter(anchor => anchor.getClientRects().length > 0)
            .map(anchor => new URL(anchor.href).pathname.replace(/\/$/, ""))
            .filter(path => /\/settings\/[^/]+$/.test(path));
          return [...new Set(paths)];
        });
        return [...routes].sort();
      },
      { message: "the settings navigation lists every page", timeout: 10_000 },
    )
    .toEqual([...Object.keys(getters), ...ERROR_ONLY_ROUTES].sort());

  // The page loaded first is audited first, whatever the navigation order, so
  // its audit covers the initial load and the session getters.
  const ordered = [route, ...routes.filter(path => path !== route)];
  let mark = 0;
  for (const path of ordered) {
    await test.step(path, async () => {
      route = path;
      const link = page.locator(`a[href$="${path}"]:visible`).first();
      const title = (await link.innerText()).trim();
      if (new URL(page.url()).pathname.replace(/\/$/, "") !== path) {
        await link.click();
        await page.waitForURL(url => url.pathname.replace(/\/$/, "") === path);
      }
      // Getter-free routes prove their mount with their matching content title.
      // Other routes prove it with their successful getters below.
      const heading = ERROR_ONLY_ROUTES.includes(path)
        ? page.getByRole("heading", { level: 2, name: title, exact: true })
        : page.getByRole("heading", { level: 2 }).first();
      await expect(heading).toBeVisible();

      // Wait for the route's getters before measuring settled traffic: a lazy
      // route can still be loading while it has sent no requests at all.
      const required = [...(getters[path] ?? [])];
      let unanswered: { id?: unknown; method?: string }[] = [];
      let answeredSince: number | null = null;
      await expect
        .poll(
          async () => {
            const log = await rpcLog(page);
            unanswered = log.unanswered(mark);
            if (unanswered.length > 0 || required.some(method => log.successes(method, mark) === 0))
              answeredSince = null;
            else answeredSince ??= Date.now();
            return answeredSince !== null && Date.now() - answeredSince >= 1000;
          },
          { message: `${path}: RPC requests must be answered`, timeout: 15_000, intervals: [250] },
        )
        .toBe(true)
        .catch(() => {
          problems.push({ route, kind: "unanswered RPC", detail: JSON.stringify(unanswered) });
        });

      const log = await rpcLog(page);
      // The Mouse page asks for the schedule only when the jiggler is on.
      if (path === "/settings/mouse" && log.answeredTrue("getJigglerState", mark))
        required.push("getJigglerConfig");
      // The Network page shows the Wi-Fi cards only on a board that reports wifi.
      if (path === "/settings/network" && capabilities.includes("wifi"))
        required.push("getWifiSettings");
      for (const method of required)
        if (log.successes(method, mark) === 0)
          problems.push({ route, kind: "missing RPC", detail: `${method} was not answered` });
      for (const event of REQUIRED_EVENTS[path] ?? [])
        if (log.events(event, mark) === 0)
          problems.push({ route, kind: "missing event", detail: `${event} did not arrive` });
      for (const { method, error } of log.errors(mark))
        problems.push({ route, kind: "RPC error", detail: `${method}: ${JSON.stringify(error)}` });
      for (const reason of log.rejections(mark))
        problems.push({ route, kind: "unhandled rejection", detail: reason });

      mark = log.mark;
      await test.info().attach(`settings${path.replace(/\//g, "-")}.png`, {
        body: await page.screenshot(),
        contentType: "image/png",
      });
    });
  }

  expect(problems, "settings pages must load without errors").toEqual([]);

  // Close the session, then check that the device admitted and served it
  // without a memory-policy refusal.
  await page.close();
  let after: Record<string, unknown> = {};
  await expect
    .poll(
      async () => {
        after = await deviceStats();
        return after.sessionActive === false && after.peerConnected === false;
      },
      { message: "the device must close the browser session", timeout: 15_000 },
    )
    .toBe(true);
  expect(memoryCounters(after), "memory-policy counters must not grow").toEqual(before);
});
